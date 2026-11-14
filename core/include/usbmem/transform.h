// SPDX-License-Identifier: Apache-2.0
// BlockTransform: 暗号化ボリューム (BitLocker / LUKS1 / LUKS2) を読み取り専用で復号する層。
//
//   BlockDevice (暗号化された生デバイス / パーティション)
//        └─ open_crypto_volume() ─→ 復号済み読み取り専用 BlockDevice ─→ probe_filesystem / mount_auto
//
// 方針 (docs/crypto-volumes.md):
//  - 書き込みは既定で常に Errc::ReadOnly で拒否する (flush は何もしない)。OpenOptions::writable を明示し、かつビルドが
//    USBMEM_CRYPT_WRITE で、構成が書き込み対応 (VolumeInfo::write_supported) のときだけ書き込み可 (is_read_only() == false)。
//    書き込み = 平文を暗号化して下の BlockDevice へ書くだけ (ヘッダ・鍵は変えない)。設計と制限は docs/crypto-volumes.md「書き込み」。
//  - 鍵・パスワード・復号済みキャッシュは crypto::Secret に置き、close() / 破棄でゼロ化する。ログには出さない
//    (describe() / VolumeInfo::note は秘密を含まない)。
//  - パスワード・回復キー違いは Errc::AuthFailed (理由の分類だけを返し、候補や途中値は返さない)。
//  - 非対応の構成 (TPM のみ・変換途中・dm-integrity・分離ヘッダなど) は黙って誤動作せず Errc::Unsupported と理由を返す。
//  - 壊れた/切り詰められたヘッダは Errc::Corrupt。サイズ・個数・KDF メモリは上限を持ち、無制限に確保しない。
#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
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
  // 復号済みセクタのキャッシュ (チャンク数 × 32 KiB)。0 で既定 (128 = 4 MiB)。
  uint32_t cache_chunks = 0;
  const CancelToken* cancel = nullptr;  // KDF (数秒かかりうる) の途中でキャンセル確認
  // 書き込み可で開くことを明示的に要求する。既定 false (読み取り専用)。true でも、(a) ビルドが USBMEM_CRYPT_WRITE でない、(b) 下位デバイスが
  // 読み取り専用、(c) 構成が書き込み非対応 (BitLocker の暗号化途中・LUKS の未検証 IV 方式など) のときは「解錠は成功するが読み取り専用」で開き、
  // VolumeInfo::writable == false と write_note に理由を返す (FS の mount_rw が読み取り専用に落ちるのと同じ流儀)。
  bool writable = false;
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
  bool write_supported = false;        // この構成 (ビルド・暗号方式・状態) なら書き込み可で開ける (資格情報・下位デバイスの書き込み可否は別)
  bool writable = false;               // open の結果: 返したデバイスは書き込み可 (is_read_only() == false)
  std::string write_note;              // 書き込み不可の理由 (要求したのに不可だったとき / probe で write_supported が false のとき)
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
  // 書き込み可 (writable_) でなければ ReadOnly。書き込み可なら [offset, offset+len) を暗号セクタ単位に広げ、端の部分セクタは
  // 「平文を読む → 変更 → 暗号化 → 下位へ書く」(RMW)、それ以外はそのまま暗号化して下位へ書く (書き込みは下位へ即時に素通し。遅延書き込みはしない)。
  // 平文ページキャッシュは書いた範囲を更新する (失敗時は無効化)。下位の書き込み / flush が失敗したら、以後は書き込みを拒否 (読み取り専用に落とす)。
  Status write_at(uint64_t offset, const void* buf, size_t len, const IoOptions& opt = {}) override;
  Status flush() override;  // 書き込み可なら下位の flush を確実に呼ぶ。読み取り専用なら何もしない
  Status close() override;  // キャッシュと鍵をゼロ化。以後の I/O は Closed
  uint64_t size() const override { return size_; }
  uint32_t logical_block_size() const override { return sector_; }
  bool is_read_only() const override { return !writable_ || write_failed_.load(); }
  std::string describe() const override { return desc_; }

 protected:
  // size はセクタの倍数に切り下げられる。sector は 512 以上の 2 のべき乗。
  DecryptingDevice(BlockDevicePtr base, uint64_t size, uint32_t sector, uint32_t cache_chunks, std::string desc);
  // [off, off+len) は sector 境界で len > 0。buf に平文を返す。複数スレッドから並行に呼ばれうる (内部状態は読み取りのみ)。
  virtual Status read_plain(uint64_t off, uint8_t* buf, size_t len, const IoOptions& opt) = 0;
  // 鍵材料をゼロ化する (close() から 1 度だけ呼ばれる。派生側のメンバは Secret / Aes なので、破棄時にも自動でゼロ化される)。
  virtual void wipe_keys() = 0;
  // ---- 書き込み (派生側が set_writable(true) したときだけ使われる)
  // [off, off+len) は sector 境界で len > 0。buf の平文を暗号化して下位へ書く。write_at は同時に 1 つだけ (読み取りとも排他) 呼ぶ。
  virtual Status write_plain(uint64_t off, const uint8_t* buf, size_t len, const IoOptions& opt) { (void)off; (void)buf; (void)len; (void)opt; return Status::Make(Errc::ReadOnly); }
  // 書いてはいけない範囲 (BitLocker のメタデータ領域など。読み取りでは 0 に見せている範囲) を含むなら ReadOnly を返す。下位へは何も書かれない。
  virtual Status check_write_range(uint64_t off, size_t len) const { (void)off; (void)len; return Status::Ok(); }
  void set_writable(bool w) { writable_ = w; }

  BlockDevicePtr base_;

 private:
  // 復号済みデータのキャッシュは「ページ」単位 (page_bytes_ = max(4096, セクタ))。小さい読み (キャッシュ経由) の欠けたページは、
  // 必要なページに加えて、連続読みと判定したときだけ先読みして 1 回の下位読み取りにまとめる (ランダム読みでは先読みしない)。
  struct Page {
    uint64_t index = 0;
    crypto::Secret data;
    size_t len = 0;
    uint64_t stamp = 0;
  };
  struct Stream {  // 連続読みの追跡 (最大 4 本。NTFS のメタデータとデータのように交互に読まれても追えるように)
    uint64_t next = UINT64_MAX;  // 次に続くはずのページ
    uint32_t ra = 0;             // 現在の先読みページ数
    uint64_t stamp = 0;
  };
  // 読み取り要求 [first_page, last_page] を追跡し、欠けたときの先読みページ数を返す (cache_mu_ を持って呼ぶ)
  uint32_t track_stream_locked(uint64_t first_page, uint64_t last_page, bool* sequential);
  void update_cache_after_write(uint64_t off, const uint8_t* data, size_t len);  // 書いた範囲のキャッシュ済みページを更新
  void invalidate_pages(uint64_t off, size_t len);                                // 範囲に掛かるページを捨てる
  uint64_t size_;
  uint32_t sector_;
  uint32_t page_bytes_;
  uint32_t max_pages_;
  std::string desc_;
  std::shared_mutex life_;  // 共有: I/O、排他: close
  std::shared_mutex io_;    // 共有: 読み取り、排他: 書き込み / flush (読み取りが古い平文をキャッシュに入れる競合を防ぐ)
  bool closed_ = false;
  bool wiped_ = false;
  bool writable_ = false;
  std::atomic<bool> write_failed_{false};  // 下位の書き込み / flush が失敗した: 以後の書き込みは拒否する (読み取りは続けられる)
  std::atomic<int> fail_code_{0};
  std::mutex cache_mu_;
  std::vector<Page> pages_;
  std::unordered_map<uint64_t, uint32_t> page_map_;  // ページ番号 -> pages_ の添字
  Stream streams_[4];
  uint64_t tick_ = 0;
};

}  // namespace usbmem
