// SPDX-License-Identifier: Apache-2.0
// ディスクイメージ形式 (QCOW2 / VHD / VHDX / VMDK) の BlockDevice、COW オーバーレイ、NBD クライアント。
// どれも BlockDevice として振る舞うので、パーティション解析 (read_partition_table) とファイルシステムのマウントがそのまま使える。
//
// 方針:
//  - イメージ形式は読み取り専用 (is_read_only() == true)。書き込みを試したいときは OverlayDevice を重ねる (元のイメージは書き換えない)。
//  - ディスク上の構造は信用しない: すべてのテーブル参照・サイズ・オフセットを検証し、確保量に上限を設ける (Errc::Corrupt)。
//  - 対応しない機能は黙って誤読せず、明示的に Errc::Unsupported で拒否する (ImageInfo::detail に理由)。
#pragma once
#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "usbmem/block_device.h"

namespace usbmem {

enum class ImageFormat : int { Raw = 0, Qcow2 = 1, Vhd = 2, Vhdx = 3, Vmdk = 4 };
const char* image_format_name(ImageFormat f);  // "raw" / "qcow2" / "vhd" / "vhdx" / "vmdk"

// イメージが参照する別ファイル (バッキング / 親 / VMDK のエクステント) を、呼び出し側が開いて返す。
// 呼び出し側が「どのファイルを開いてよいか」を決める (イメージ内の名前は信用しない: 絶対パスや ".." を含みうる)。
// 返すデバイスは生のファイル (RawImageDevice など)。形式の解釈 (qcow2 のバッキングが qcow2 など) はこちらで行う。
enum class ResolveKind { Backing, Extent };
struct ImageResolveRequest {
  ResolveKind kind = ResolveKind::Backing;
  std::string name;                   // イメージに書かれている名前 (UTF-8、区切りは '/' に正規化済み)
  std::vector<std::string> alt_names;  // 別の候補 (VHD の親パスの複数表記など)
  std::string format_hint;            // "qcow2" / "raw" / "vmdk" ... 空なら不明 (QCOW2 の backing_fmt など)
};
using ImageResolver = std::function<Status(const ImageResolveRequest&, BlockDevicePtr* out)>;

struct ImageOpenOptions {
  ImageResolver resolver;               // 空なら、別ファイルが必要なイメージは Unsupported で拒否
  uint32_t max_backing_depth = 8;       // バッキング/親チェーンの深さ上限 (循環もここで止まる)
  size_t max_cache_bytes = 16u << 20;   // 1 イメージあたりのテーブルキャッシュ上限
  bool allow_unreplayed_log = false;    // VHDX: ログが未再生でも読む (内容が古い可能性あり)。既定は拒否
  bool strict_checksums = true;         // VHD / VHDX のヘッダチェックサム不一致を拒否
  bool allow_parent_mismatch = false;   // 差分イメージの親 ID 不一致を許す
  bool probe_backing_format = true;     // バッキングの形式が書かれていないとき、先頭のマジックで判別する
};

struct ImageInfo {
  ImageFormat format = ImageFormat::Raw;
  std::string subformat;               // 例: "qcow2 v3" / "vhd dynamic" / "vmdk streamOptimized"
  uint64_t virtual_size = 0;
  uint32_t logical_block_size = 512;
  bool has_parent = false;             // バッキング/親あり
  std::string parent_name;
  bool compressed = false;             // 圧縮されたデータを含みうる形式
  std::string content_id;              // 差分の親検証用の ID (16 進。VHD の unique id / VHDX の Virtual Disk ID / VMDK の CID)
  std::string detail;                  // 失敗時: 人間向けの理由 (英語)。成功時: 注意事項 (空のこともある)
};

// 先頭/末尾のマジックで形式を判別する。判別できなければ Raw (RAW は最後のフォールバック)。
// VHD (固定) は末尾のフッタ (チェックサム検証つき) でだけ判別するので、RAW を VHD と誤認しない。
Status detect_image_format(BlockDevice& file, ImageFormat* out);

// file の形式を判別して開く。Raw のときは file をそのまま返す。失敗時 info->detail に理由。
Status open_disk_image(BlockDevicePtr file, const ImageOpenOptions& opts, BlockDevicePtr* out, ImageInfo* info = nullptr);

// 形式を指定して開く (判別を行わない)。
Status open_qcow2_image(BlockDevicePtr file, const ImageOpenOptions& opts, BlockDevicePtr* out, ImageInfo* info = nullptr);
Status open_vhd_image(BlockDevicePtr file, const ImageOpenOptions& opts, BlockDevicePtr* out, ImageInfo* info = nullptr);
Status open_vhdx_image(BlockDevicePtr file, const ImageOpenOptions& opts, BlockDevicePtr* out, ImageInfo* info = nullptr);
Status open_vmdk_image(BlockDevicePtr file, const ImageOpenOptions& opts, BlockDevicePtr* out, ImageInfo* info = nullptr);

// パスから開く便利関数 (ホスト・テスト用)。バッキング等は、イメージのあるディレクトリからの相対パスだけ解決する
// (絶対パス・".." を含む名前は allow_unsafe_paths=false なら拒否)。fd の所有権は返したデバイスが持つ。
Status open_disk_image_path(const std::string& path, bool allow_unsafe_paths, BlockDevicePtr* out, ImageInfo* info = nullptr);

// ---------------------------------------------------------------- COW オーバーレイ
// 読み取り専用のベースの上に、書き込みを別の記憶 (メモリ / ファイル) へ溜める。ベースには決して書かない。
//  - 書き込みはブロック (既定 4096 B) 単位で追記ログに記録する。レコードは CRC 付きなので、クラッシュで末尾が切れても、
//    再オープン時に壊れたレコード以降を捨てて「それまでの書き込み」に戻る (ベースは無傷)。
//  - commit() はベース (別の書き込み可能デバイス) へ書き戻す。明示的な操作で、書き込みの途中で失敗してもオーバーレイは残る。
//  - discard() は溜めた書き込みを全部捨てる。
class OverlayStore;
class OverlayDevice final : public BlockDevice {
 public:
  struct Options {
    uint32_t block_size = 4096;   // 2 の冪、ベースの論理ブロックサイズの倍数
    std::string file_path;        // 空ならメモリ上。指定すればそのファイルに追記ログとして保存 (既存なら再開)
    bool sync_on_flush = true;
    uint64_t max_overlay_bytes = 0;  // 0 = 無制限。超えると Io(ENOSPC)
  };
  static Status create(BlockDevicePtr base, const Options& opt, std::shared_ptr<OverlayDevice>* out);
  ~OverlayDevice() override;

  Status read_at(uint64_t, void*, size_t, const IoOptions& = {}) override;
  Status write_at(uint64_t, const void*, size_t, const IoOptions& = {}) override;
  Status flush() override;
  Status close() override;
  uint64_t size() const override { return size_; }
  uint32_t logical_block_size() const override { return base_->logical_block_size(); }
  bool is_read_only() const override { return false; }
  std::string describe() const override;

  uint64_t dirty_blocks() const;
  uint64_t dirty_bytes() const { return dirty_blocks() * block_size_; }
  // 溜めた書き込みを target (書き込み可能・ベースと同じサイズ以上) へ書き戻して flush する。成功してもオーバーレイは残す (clear_after=true で破棄)。
  Status commit(BlockDevice& target, bool clear_after = false, const IoOptions& opt = {});
  Status discard();

 private:
  OverlayDevice(BlockDevicePtr base, uint32_t bs);
  struct Impl;
  std::unique_ptr<Impl> impl_;
  BlockDevicePtr base_;
  uint64_t size_;
  uint32_t block_size_;
};

// ---------------------------------------------------------------- NBD クライアント
// Network Block Device (https://github.com/NetworkBlockDevice/nbd/blob/master/doc/proto.md)。fixed newstyle のみ (旧形式は拒否)。TLS は未対応。
// 1 接続・同時 1 要求 (内部で直列化)。切断・タイムアウト後は、auto_reconnect なら次の I/O で再接続して 1 回だけ再試行する。
struct NbdOptions {
  std::string host;            // ホスト名 / IP。unix_socket が空でないときは無視
  uint16_t port = 10809;
  std::string unix_socket;     // パス (Linux では先頭 '@' なら抽象名前空間)
  std::string export_name;     // 空 = 既定のエクスポート
  bool read_only = false;      // true なら書き込みを拒否 (サーバが書き込みを許してもこちらで止める)
  uint32_t connect_timeout_ms = 10000;
  uint32_t io_timeout_ms = 30000;     // IoOptions::timeout_ms が 0 のときの既定
  bool auto_reconnect = true;
  uint32_t max_request_bytes = 4u << 20;   // 1 要求の最大 (サーバの最大ブロックサイズが小さければそちらに従う)
  bool use_structured_replies = true;
  // true (既定): サーバが FUA に対応していれば全書き込みに FUA を付ける。再接続しても「成功した書き込み」が失われない。
  // false: FUA を使わない。再接続をまたいだ未 flush の書き込みは失われうるので、次の flush() が Disconnected を返して知らせる。
  bool fua_writes = true;
};

class NbdDevice : public BlockDevice {
 public:
  static Status connect(const NbdOptions& opt, std::shared_ptr<NbdDevice>* out, std::string* detail = nullptr);
  // エクスポート名の一覧 (NBD_OPT_LIST)。
  static Status list_exports(const NbdOptions& opt, std::vector<std::string>* names, std::string* detail = nullptr);
  // 未割り当て/ゼロ化を要求する拡張操作 (サーバが対応していなければ Unsupported)。
  virtual Status trim(uint64_t offset, uint64_t len, const IoOptions& opt = {}) = 0;
  virtual Status write_zeroes(uint64_t offset, uint64_t len, const IoOptions& opt = {}) = 0;
  virtual uint32_t min_block_size() const = 0;
  virtual uint32_t max_block_size() const = 0;
  virtual uint16_t transmission_flags() const = 0;
  virtual bool structured_replies() const = 0;
  virtual uint32_t reconnect_count() const = 0;
};

}  // namespace usbmem
