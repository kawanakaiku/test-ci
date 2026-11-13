// SPDX-License-Identifier: Apache-2.0
// USB Mass Storage (Bulk-Only Transport + SCSI transparent command set) を BlockDevice として提供する。
// OS 依存の転送層は BulkTransport に分離する (Android: usbdevfs / テスト: ソフトウェア模擬デバイス)。
// 仕様: USB Mass Storage Class Bulk-Only Transport 1.0, SCSI SPC-4 / SBC-3 (INQUIRY, TEST UNIT READY, REQUEST SENSE,
// READ CAPACITY(10/16), MODE SENSE(6), READ/WRITE(10/16), SYNCHRONIZE CACHE(10))。
#pragma once
#include <atomic>
#include <cstdint>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "usbmem/block_device.h"

namespace usbmem {

enum class UsbDir { In, Out };

// 1 つの Mass Storage インタフェースの bulk エンドポイント対。実装はスレッドセーフでなくてよい (MscDevice が直列化する)。
class BulkTransport {
 public:
  /** 1 回の bulk 転送要求の最大バイト数 (実装が分割するときの単位)。方向別。実行時の調整用で、既定は実装依存。 */
  virtual void set_chunk(UsbDir dir, size_t bytes) { (void)dir; (void)bytes; }
  virtual ~BulkTransport() = default;
  // 転送できたバイト数を *done へ。短い転送は Ok のまま done < len。
  // STALL は Errc::Io + sys_errno=EPIPE、タイムアウトは Errc::Timeout、抜去は Errc::Disconnected。
  virtual Status bulk_out(const void* buf, size_t len, size_t* done, uint32_t timeout_ms) = 0;
  virtual Status bulk_in(void* buf, size_t len, size_t* done, uint32_t timeout_ms) = 0;
  virtual Status clear_halt(UsbDir ep) = 0;
  // クラス要求 Bulk-Only Mass Storage Reset (bRequest 0xFF)。
  virtual Status mass_storage_reset() = 0;
  // クラス要求 GET MAX LUN (bmRequestType 0xA1, bRequest 0xFE, wValue 0, wIndex = インタフェース, wLength 1)。*max_lun = 最大の LUN 番号 (LUN 数 - 1)。
  // 制御転送の STALL は Errc::Io + sys_errno=EPIPE (= LUN 1 つの機器が返す正常な応答)。既定は未対応 (Unsupported)。
  virtual Status get_max_lun(uint8_t* max_lun) { (void)max_lun; return Status::Make(Errc::Unsupported); }
};

struct MscOptions {
  uint8_t lun = 0;
  bool read_only = true;                  // false でも書き込み禁止メディア (WP) なら読み取り専用になる
  uint32_t max_transfer = 64 * 1024;      // 1 コマンドあたりの最大バイト数 (ブロックサイズの倍数に丸める)。書き込みは常にこの値
  uint32_t max_read_transfer = 0;         // 読み取り 1 コマンドあたりの最大バイト数。0 = max_transfer。読み取りだけ大きくして速度を上げられる (書き込みは変えない)
  uint32_t io_timeout_ms = 10000;
  uint32_t ready_wait_ms = 10000;         // TEST UNIT READY を待つ上限
  uint32_t retry_delay_ms = 200;
  uint32_t probe_timeout_ms = 5000;       // 種別判別 (INQUIRY EVPD / ATA IDENTIFY) の 1 コマンドあたりの上限。再試行はしない
  bool probe_media = true;                // false なら種別判別をしない (MediaKind::Unknown のまま)。既定は判別する
};

// SCSI SENSE の要約 (診断・エラー対応表示用)
struct ScsiSense { uint8_t key = 0, asc = 0, ascq = 0; };

// ---- 媒体の種別判別 (回転ディスクか、フラッシュ/SSD か)。docs/media-kind.md
enum class MediaKind : int { Unknown = 0, Rotational = 1, NonRotational = 2 };
// 判定の根拠 (どの方法で分かったか)
enum class MediaBasis : int {
  None = 0,
  VpdBlockDeviceChars = 1,   // INQUIRY EVPD 0xB1 (Block Device Characteristics) の MEDIUM ROTATION RATE
  AtaInfoVpd = 2,            // INQUIRY EVPD 0x89 (ATA Information) に入っている IDENTIFY DEVICE の語 217
  AtaPassThrough12 = 3,      // SAT ATA PASS-THROUGH(12) で ATA IDENTIFY DEVICE の語 217
  AtaPassThrough16 = 4,      // SAT ATA PASS-THROUGH(16) で同上
};
// 判定できなかったときの手掛かり (kind が Unknown のときの補助。確定ではない)
enum class MediaHint : int {
  None = 0,
  RemovableNoVpd = 1,        // RMB=1 (リムーバブル) で回転数の情報が得られない: USB メモリ (フラッシュ) の可能性が高い
  UnmapSupported = 2,        // UNMAP/TRIM に対応 (VPD 0xB2 の LBPU / IDENTIFY 語 169): SSD・フラッシュのヒント (シン・プロビジョニングの RAID もありうる)
};

struct MediaInfo {
  MediaKind kind = MediaKind::Unknown;
  uint32_t rpm = 0;                       // kind == Rotational で回転数が分かるとき (0 = 回転数は不明)
  MediaBasis basis = MediaBasis::None;
  MediaHint hint = MediaHint::None;
  // ---- 診断用の生の値 (判別できなかったときの調査用)
  bool probed = false;                    // 判別を実行した (false = probe_media=false など)
  bool removable = false;                 // 標準 INQUIRY の RMB
  uint8_t inquiry_version = 0;            // 標準 INQUIRY のバイト 2 (VERSION)
  uint8_t inquiry_ptype = 0;              // 標準 INQUIRY のペリフェラルデバイスタイプ (VPD 応答の検査用)
  bool vpd_tried = false;                 // VPD 0x00 を問い合わせた (INQUIRY の VERSION が SPC-2 未満の機器には問い合わせない)
  bool vpd_supported = false;             // VPD 0x00 に正常に応答した
  std::vector<uint8_t> vpd_pages;         // 対応ページ一覧 (VPD 0x00)
  int32_t rotation_rate_raw = -1;         // VPD 0xB1 の MEDIUM ROTATION RATE (-1 = 取得していない)
  int32_t unmap_raw = -1;                 // VPD 0xB2 の LBPU (0/1, -1 = 取得していない)
  int32_t ata_word0 = -1, ata_word169 = -1, ata_word217 = -1;   // ATA IDENTIFY DEVICE の語 (-1 = 取得していない)
  uint8_t sat_tried = 0;                  // 試した SAT: bit0 = 0x89, bit1 = PASS-THROUGH(12), bit2 = PASS-THROUGH(16)
  bool sat_skipped = false;               // RMB=1 で VPD 0x89 も無いため、ATA PASS-THROUGH を送らなかった (USB メモリへの未知コマンドを避ける)
  std::string stop_reason;                // 判別を途中で諦めた理由 (空 = 最後まで進んだ)
  /** 診断用の 1 行要約 (例: "rmb=0 ver=6 vpd=00,80,83,b0,b1,b2 b1=0x1c20 -> rotational 7200rpm [vpd-b1]")。ASCII のみ。 */
  std::string describe() const;
};

/** 読み取り専用モードで BOT に流してよい CDB か (最後の安全弁。MscDevice::exec が使う。単体テストから直接検査できるよう公開)。
 *  - 許可: TEST UNIT READY / REQUEST SENSE / INQUIRY (標準と EVPD。CMDDT は不可) / MODE SENSE(6) / READ CAPACITY(10) / READ(10,16) / SERVICE ACTION IN(16) の READ CAPACITY(16)
 *  - START STOP UNIT (0x1B) は allow_start_stop のときだけ
 *  - ATA PASS-THROUGH(12) (0xA1) / (16) (0x85) は allow_ata_identify のときだけ、**IDENTIFY DEVICE (0xEC) の正規形のみ**
 *    (PIO データイン・拡張なし・512 バイト・レジスタはすべて 0 / セクタ数 1)。それ以外の ATA コマンドは常に拒否する。 */
bool scsi_cdb_allowed_read_only(const uint8_t* cdb, size_t cdb_len, bool allow_start_stop, bool allow_ata_identify);
/** ATA PASS-THROUGH の CDB を、書き込み可のモードでも制限するための検査: 0xA1/0x85 なら IDENTIFY DEVICE の正規形だけ true。他のオペコードは true (対象外)。 */
bool scsi_cdb_ata_passthrough_ok(const uint8_t* cdb, size_t cdb_len, bool allow_ata_identify);
/** ATA IDENTIFY DEVICE の 512 バイトを検査し、語 217 (NOMINAL MEDIA ROTATION RATE) などを取り出す。
 *  不正 (ATAPI・チェックサム不一致など) なら false。語 255 の下位バイトが 0xA5 のときだけチェックサム (全バイトの和 = 0) を検査する。 */
bool parse_ata_identify(const uint8_t* data512, MediaInfo* out);

// ---- 複数 LUN (論理ユニット)。docs/multi-usb.md の LUN の節
// 1 つの LUN の状態 (LUN 一覧)。判別に使うコマンドは INQUIRY / TEST UNIT READY / REQUEST SENSE / READ CAPACITY だけ (読み取り専用の許可リスト内)。
enum class LunState : int {
  Ready = 0,        // メディアあり・容量が分かった (開ける)
  NoMedia = 1,      // メディアなし (カードが入っていない)
  NotReady = 2,     // 準備中のまま待ち時間を超えた (NOT READY / becoming ready)
  Unsupported = 3,  // 直接アクセス型ディスクではない (光学ドライブなど)、または LUN が接続されていない
  Error = 4,        // コマンドが失敗した (note に理由)
  Skipped = 5,      // 調べなかった (スキップ指定。すでに開いている LUN に余計なコマンドを送らないため)
};
struct LunInfo {
  uint8_t lun = 0;
  LunState state = LunState::Error;
  uint8_t ptype = 0;             // INQUIRY のペリフェラルデバイスタイプ
  bool removable = false;        // INQUIRY の RMB
  std::string vendor, product;
  uint64_t blocks = 0;           // state == Ready のとき
  uint32_t block_size = 0;
  std::string note;              // Error / NotReady / Unsupported の理由 (診断用。ASCII)
  uint64_t size() const { return blocks * block_size; }
};
const char* lun_state_name(LunState s);

class MscDevice;

/**
 * 1 つの Mass Storage インタフェース (= 1 本の USB 接続) を、複数の LUN で共有するための土台。BulkTransport を所有し、
 * **BOT のコマンド (CBW → データ → CSW) を 1 つずつ直列化する** (bulk エンドポイントは LUN で共有なので、別の LUN の I/O が割り込むと壊れる)。
 * 切断 (dead) も共有する。LUN ごとの [MscDevice] は shared_ptr でこれを保持する (bus を閉じた後も、デバイスが生きている間はトランスポートも生きる)。
 */
class MscBus : public std::enable_shared_from_this<MscBus> {
 public:
  /** query_max_lun = true なら GET MAX LUN を**1 回だけ**送る。STALL・エラー・不正な値 (>= 16) は LUN 1 つとみなし、失敗にしない。 */
  static Status create(std::unique_ptr<BulkTransport> transport, bool query_max_lun, std::shared_ptr<MscBus>* out);
  ~MscBus() = default;
  /** LUN の数 (1..16)。GET MAX LUN を問い合わせていなければ 1。 */
  uint32_t lun_count() const { return lun_count_; }
  /** GET MAX LUN の結果の診断用 1 行 ("not-queried" / "max_lun=1" / "stall" / "error:..." / "bogus:..." / "short-reply")。 */
  const std::string& max_lun_note() const { return lun_note_; }
  bool dead() const { return dead_; }
  /** LUN を開く (INQUIRY → TEST UNIT READY → READ CAPACITY…)。opt.lun が LUN 番号。問い合わせ済みで範囲外なら InvalidArgument。 */
  Status open_lun(const MscOptions& opt, BlockDevicePtr* out);
  /** 全 LUN を調べる (読み取り専用のコマンドだけ)。skip_mask の bit n が立っている LUN は調べず Skipped にする。
   *  メディアなし・未対応・個別のコマンド失敗は LUN ごとの状態で表し、戻り値は成功。切断されたときだけ Disconnected を返す。 */
  Status scan_luns(const MscOptions& opt, uint32_t skip_mask, std::vector<LunInfo>* out);

 private:
  friend class MscDevice;
  explicit MscBus(std::unique_ptr<BulkTransport> t) : t_(std::move(t)) {}
  std::unique_ptr<BulkTransport> t_;
  std::mutex cmd_mu_;               // 1 コマンド (CBW → データ → CSW、失敗時のリセット回復まで) の間ずっと持つ
  std::atomic<bool> dead_{false};
  uint32_t tag_ = 1;                // cmd_mu_ の下で更新
  uint32_t lun_count_ = 1;
  std::string lun_note_ = "not-queried";
};

class MscDevice final : public BlockDevice {
 public:
  // transport の所有権を取る。INQUIRY → TEST UNIT READY → READ CAPACITY まで行い、失敗時は out を設定しない。opt.lun の LUN を開く
  // (GET MAX LUN は送らない: 従来の単一 LUN の使い方)。複数 LUN を扱うときは MscBus を使う。
  static Status open(std::unique_ptr<BulkTransport> transport, const MscOptions& opt, BlockDevicePtr* out);

  Status read_at(uint64_t, void*, size_t, const IoOptions& = {}) override;
  Status write_at(uint64_t, const void*, size_t, const IoOptions& = {}) override;
  Status flush() override;
  Status close() override;
  uint64_t size() const override { return blocks_ * block_size_; }
  uint32_t logical_block_size() const override { return block_size_; }
  bool is_read_only() const override { return read_only_; }
  std::string describe() const override;

  /** ディスクの回転を止める (start=false) / 再開する (start=true)。SCSI START STOP UNIT (LOEJ=0: メディアは取り出さない)。
   *  メディアの内容は変えないので、読み取り専用でも送れる (専用のこの関数の中だけで許可リストを一時的に緩める)。
   *  書き込み可で止める前には SYNCHRONIZE CACHE を送る。未対応のデバイスは Unsupported。止めた後は、再開 (start=true) するまで READ は失敗しうる。 */
  Status start_stop(bool start);
  /** アイドル自動スピンダウン用。最後の I/O から idle_ms 以上たっていて、まだ停止していなければ止める (I/O 中は待たずに 0 を返す)。
   *  戻り値: 1 = 止めた / 0 = 何もしなかった (まだ使用中・すでに停止中・非対応と判明済み・I/O 実行中) / <0 = -Errc (未対応は Unsupported。以後は試さない)。
   *  止めた後の READ/WRITE/flush は、先に自動で START を送ってから実行する (呼び出し側は何もしなくてよい)。 */
  int idle_stop(uint64_t idle_ms);
  bool spun_down() const;

  ScsiSense last_sense() const { return last_sense_; }
  /** 送った SCSI オペコードの集計 (例: "12x1 1ax1 25x1 28x4237")。読み取り専用で WRITE 系が 0 であることの実機での証拠。 */
  std::string op_stats() const;
  /** **読み取り**の転送サイズの調整 (速度計測・チューニング用。書き込みの設定は変えない)。0 = 変更しない。
   *  read_chunk = 1 回の USB 転送要求 (IN, 16KiB の倍数)、read_max_transfer = 1 SCSI READ コマンドあたり (ブロック長の倍数に丸める)。 */
  void tune(size_t read_chunk, uint32_t read_max_transfer);
  /** 診断用: 直近の open() 中に送った SCSI コマンドと結果の記録 (失敗時の原因調査用)。**呼んだスレッドで open した分**だけ (スレッドごと)。 */
  static std::string last_open_trace();
  uint32_t op_count(uint8_t opcode) const { return op_counts_[opcode]; }
  bool write_protected() const { return write_protected_; }
  const std::string& vendor() const { return vendor_; }
  const std::string& product() const { return product_; }
  /** 回転ディスクか、フラッシュ/SSD か。open() の中で 1 度だけ判別して保持する (以後コマンドは送らない)。判別できなければ Unknown。 */
  const MediaInfo& media_info() const { return media_; }
  uint8_t lun() const { return opt_.lun; }

 private:
  friend class MscBus;
  MscDevice(std::shared_ptr<MscBus> bus, const MscOptions& o) : bus_(std::move(bus)), opt_(o) {}
  struct Cmd;
  Status exec(const uint8_t* cdb, uint8_t cdb_len, UsbDir dir, void* data, size_t len, size_t* transferred, uint32_t timeout_ms, bool* check_condition);
  Status exec_checked(const uint8_t* cdb, uint8_t cdb_len, UsbDir dir, void* data, size_t len, uint32_t timeout_ms, size_t min_len = SIZE_MAX);
  Status request_sense(ScsiSense* s);
  Status reset_recovery();
  Status rw_blocks(bool write, uint64_t lba, uint32_t nblocks, void* buf, const IoOptions& io);
  Status read_locked(uint64_t off, void* buf, size_t len, const IoOptions& io);
  Status write_locked(uint64_t off, const void* buf, size_t len, const IoOptions& io);
  Status initialize();
  Status inquiry(uint8_t* inq36);
  Status wait_ready(uint32_t wait_ms);
  Status read_capacity(uint64_t* blocks, uint32_t* block_size);
  Status probe_lun(LunInfo* li, uint32_t ready_wait_ms);   // MscBus::scan_luns 用: 1 つの LUN の状態 (切断以外は戻り値 Ok で li->state に)
  Status map_sense(const ScsiSense& s);

  std::shared_ptr<MscBus> bus_;
  MscOptions opt_;
  mutable std::mutex mu_;
  uint64_t blocks_ = 0;
  uint32_t block_size_ = 0;
  bool read_only_ = true, write_protected_ = false, closed_ = false;
  bool spun_down_ = false, stop_unsupported_ = false;
  std::chrono::steady_clock::time_point last_io_ = std::chrono::steady_clock::now();
  Status start_stop_locked(bool start);
  Status resume_if_stopped_locked();   // I/O の前に呼ぶ: 停止中なら START を送る。最終 I/O 時刻も更新
  bool allow_start_stop_ = false;   // start_stop() の実行中だけ true (読み取り専用の許可リストに START STOP UNIT を通す)
  bool allow_ata_identify_ = false; // probe_media() の IDENTIFY 送信中だけ true (ATA PASS-THROUGH は IDENTIFY DEVICE の正規形だけを通す)
  MediaInfo media_;
  // 種別判別。1 コマンド 1 回だけ (再試行なし)、probe_timeout_ms の上限。失敗は Unknown にして open は続ける (Disconnected だけは伝える)。
  struct ProbeResult { enum Kind { Ok, IllegalRequest, Abort } kind = Abort; size_t n = 0; };
  ProbeResult probe_cmd(const uint8_t* cdb, uint8_t cdb_len, void* data, size_t len, const char* what);
  bool probe_vpd(uint8_t page, uint8_t* buf, size_t len, size_t min_len, ProbeResult* res);
  void probe_media(const uint8_t* inq36);
  ScsiSense last_sense_;
  uint32_t op_counts_[256] = {0};
  std::string trace_;
  char phase_ = '-';
  Status exec_impl(const uint8_t* cdb, uint8_t cdb_len, UsbDir dir, void* data, size_t len, size_t* transferred, uint32_t timeout_ms, bool* check_condition);
  std::string vendor_, product_;
};

}  // namespace usbmem
