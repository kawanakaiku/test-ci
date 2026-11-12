// SPDX-License-Identifier: Apache-2.0
// BlockTransform: 暗号化ボリューム (BitLocker / LUKS1 / LUKS2) を読み取り専用で復号する層。
//
//   BlockDevice (暗号化された生デバイス / パーティション)
//        └─ open_crypto_volume() ─→ 復号済み読み取り専用 BlockDevice ─→ probe_filesystem / mount_auto
//
// 方針 (docs/crypto-volumes.md):
//  - 書き込みは常に Errc::ReadOnly で拒否する。flush は何もしない。
//  - 鍵・パスワード・復号済みキャッシュは crypto::Secret に置き、close() / 破棄でゼロ化する。ログには出さない
//    (describe() / VolumeInfo::note は秘密を含まない)。
//  - パスワード・回復キー違いは Errc::AuthFailed (理由の分類だけを返し、候補や途中値は返さない)。
//  - 非対応の構成 (TPM のみ・変換途中・dm-integrity・分離ヘッダなど) は黙って誤動作せず Errc::Unsupported と理由を返す。
//  - 壊れた/切り詰められたヘッダは Errc::Corrupt。サイズ・個数・KDF メモリは上限を持ち、無制限に確保しない。
#pragma once
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

#include "usbmem/block_device.h"
#include "usbmem/crypto.h"

namespace usbmem {

enum class CryptoKind : int32_t { Auto = 0, BitLocker = 1, Luks = 2 };  // C API / JNI と同じ値

// 解錠に使う資格情報。呼び出し側のバッファからコピーされ、Secret として保持される (破棄でゼロ化)。
struct Credentials {
  crypto::Secret password;      // UTF-8 のパスワード / パスフレーズ
  crypto::Secret recovery_key;  // BitLocker の回復パスワード (48 桁、'-' 区切りは有っても無くてもよい)
  crypto::Secret key_file;      // BitLocker の .BEK ファイルの中身 / LUKS のキーファイルの中身
};

struct OpenOptions {
  // BitLocker で暗号化/復号が途中のボリュームも、変換済み領域だけ復号して開く。既定は拒否 (Unsupported)。
  bool allow_partial_encryption = false;
  // LUKS2 argon2 の memory 上限 (KiB)。これを超える keyslot は試さない (端末のメモリ不足防止)。
  uint32_t max_kdf_memory_kib = 1u << 20;  // 1 GiB
  // LUKS の PBKDF2 反復回数 / Argon2 時間コストの上限。超える keyslot は試さない (壊れた/悪意あるヘッダで何分も固まらないように)。
  uint32_t max_pbkdf2_iterations = 20000000;
  uint32_t max_argon2_time_cost = 64;
  // 復号済みセクタのキャッシュ (チャンク数 × 32 KiB)。0 で既定 (32)。
  uint32_t cache_chunks = 0;
  const CancelToken* cancel = nullptr;  // KDF (数秒かかりうる) の途中でキャンセル確認
};

// 資格情報なしで読めるヘッダ情報 (UI の分岐用)。秘密は含まない。
struct VolumeInfo {
  CryptoKind kind = CryptoKind::Auto;  // BitLocker / Luks (検出できなければ Auto)
  std::string format;                  // "BitLocker (Windows 7+)" / "LUKS1" / "LUKS2"
  std::string cipher;                  // "XTS-AES-128" / "aes-xts-plain64" など
  bool supported = false;              // このアプリがデータ層まで復号できる構成か
  bool can_password = false;           // パスワード / パスフレーズで解錠できる保護がある
  bool can_recovery_key = false;       // BitLocker: 回復パスワードで解錠できる保護がある
  bool can_key_file = false;           // BitLocker: スタートアップキー (.BEK) / LUKS: キーファイル
  bool unlocked_without_secret = false;// BitLocker: 保護一時停止 (クリアキー) で資格情報なしに開ける
  bool tpm_only = false;               // BitLocker: 保護が TPM 系のみ (このアプリでは解錠できない)
  bool encryption_in_progress = false; // BitLocker: 暗号化/復号が途中
  std::string note;                    // 人に見せる補足 (非対応の理由など。秘密を含まない)
};

// 暗号ボリュームかを判定し VolumeInfo を埋める。暗号ボリュームでなければ Unsupported (info->kind == Auto)。
// 読み取り失敗は Io 系のコードを返す。資格情報は使わない。
Status probe_crypto_volume(BlockDevice& dev, VolumeInfo* info);

// 解錠して復号済みの読み取り専用デバイスを *out に返す。
//  - kind == Auto なら probe の結果に従う。
//  - 資格情報が合わなければ AuthFailed。*info が非 null なら、可能な範囲で probe と同じ情報を返す。
//  - base は shared_ptr で保持される。out->close() は base を閉じない (所有者が閉じる)。
Status open_crypto_volume(BlockDevicePtr base, CryptoKind kind, const Credentials& cred, const OpenOptions& opt, BlockDevicePtr* out,
                          VolumeInfo* info = nullptr);

// ---- 実装用の基底 (BitLocker / LUKS が共有)。セクタ単位の平文キャッシュ・範囲検査・書き込み拒否・ゼロ化を担う。
class DecryptingDevice : public BlockDevice {
 public:
  ~DecryptingDevice() override;
  Status read_at(uint64_t offset, void* buf, size_t len, const IoOptions& opt = {}) override;
  Status write_at(uint64_t, const void*, size_t, const IoOptions& = {}) override { return Status::Make(Errc::ReadOnly); }
  Status flush() override { return Status::Ok(); }
  Status close() override;  // キャッシュと鍵をゼロ化。以後の I/O は Closed
  uint64_t size() const override { return size_; }
  uint32_t logical_block_size() const override { return sector_; }
  bool is_read_only() const override { return true; }
  std::string describe() const override { return desc_; }

 protected:
  // size はセクタの倍数に切り下げられる。sector は 512 以上の 2 のべき乗。
  DecryptingDevice(BlockDevicePtr base, uint64_t size, uint32_t sector, uint32_t cache_chunks, std::string desc);
  // [off, off+len) は sector 境界で len > 0。buf に平文を返す。複数スレッドから並行に呼ばれうる (内部状態は読み取りのみ)。
  virtual Status read_plain(uint64_t off, uint8_t* buf, size_t len, const IoOptions& opt) = 0;
  // 鍵材料をゼロ化する (close() から 1 度だけ呼ばれる。派生側のメンバは Secret / Aes なので、破棄時にも自動でゼロ化される)。
  virtual void wipe_keys() = 0;

  BlockDevicePtr base_;

 private:
  struct Chunk {
    uint64_t index;
    crypto::Secret data;
    size_t len;
    uint64_t stamp;
  };
  Status read_chunk_locked(uint64_t index, const uint8_t** data, size_t* len, const IoOptions& opt);
  uint64_t size_;
  uint32_t sector_;
  uint32_t chunk_bytes_;
  uint32_t max_chunks_;
  std::string desc_;
  std::shared_mutex life_;  // 共有: I/O、排他: close
  bool closed_ = false;
  bool wiped_ = false;
  std::mutex cache_mu_;
  std::vector<Chunk> cache_;
  uint64_t tick_ = 0;
};

}  // namespace usbmem
