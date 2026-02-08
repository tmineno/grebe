# Grebe — Vulkan 高速時系列ストリーム描画 PoC/MVP 要件定義書

**バージョン:** 1.6.0
**最終更新:** 2026-02-08

---

## 1. 目的と背景

### 1.1 目的

Vulkan を用いた時系列データストリームの高速描画パイプラインの技術的限界を検証する PoC/MVP を構築する。最終的なゴールはリアルタイム信号可視化基盤の実現可能性評価であり、本PoCではパイプライン各段のスループット・レイテンシを定量的に計測してボトルネックを特定するとともに、高速ストリーミング条件下で表示波形が入力信号を忠実に再現していることを定量的に検証する。

### 1.2 背景・動機

- 16bit/1GSPS クラスの高速 ADC データの可視化需要
- 既存ライブラリ（ImPlot, PyQtGraph 等）の性能上限を超える描画レートの探索
- CPU↔GPU 間データ転送、GPU 描画、間引きアルゴリズムそれぞれのスループット上限の把握

### 1.3 スコープ

**実装済み:**
- Vulkan 描画パイプライン（データ転送→描画→表示）
- 合成データ生成（最大 1 GSPS, period tiling による高速生成）
- MinMax (SSE2 SIMD) / LTTB 間引きアルゴリズム（≥100 MSPS で LTTB 自動無効化）
- GPU Compute Shader 間引き実験
- 複数チャンネル同時表示（1-8ch）
- パイプライン各段の計測・プロファイリング基盤
- 独立マイクロベンチマーク (BM-A, B, C, E)
- ウィンドウ/UI（波形表示 + ImGui メトリクス HUD）
- Windows ネイティブ MSVC ビルド（WSL2 経由）
- 実行バイナリ分離: `grebe` (可視化) / `grebe-sg` (信号生成)、anonymous pipe IPC
- `grebe` から `grebe-sg` の子プロセス自動起動 + `--embedded` in-process モード
- SG 専用 UI（レート / 波形 / ブロック長 / Pause）と Main 可視化 UI の責務分離
- 波形表示整合性検証（envelope 100%、sequence continuity、window coverage）

**本PoCに含まないもの:**
- 実デバイス（ADC/FPGA）との接続
- 保存・再生機能
- 本格的な UI/UX（メニュー、設定画面等）
- ネットワーク経由のデータ受信

**延期・将来マイルストーン:**
- Shared Memory IPC（TI-08 で延期判定。pipe transport で PoC 要件充足）
- `--attach-sg` attach モード（既存 `grebe-sg` への接続。現在は spawn + `--embedded` のみ）
- Trigger 機構（internal/external/timer — Phase 15+ scope）
- E2E レイテンシ定量計測（Phase 12 scope）

---

## 2. 前提条件

### 2.1 ターゲットデータ仕様

| 項目 | 値 | 備考 |
|---|---|---|
| サンプリングレート | 1 GSPS (10⁹ samples/sec) | 達成済み |
| 量子化ビット数 | 16 bit (int16_t) | 2 bytes/sample |
| データレート | 2 GB/s (1ch) | 1G × 2 bytes |
| チャンネル数 | 1-8 | `--channels=N` で指定 |
| データ形式 | リトルエンディアン int16 連続配列 | メモリ上のバイナリ列 |

### 2.2 ターゲット環境

| 項目 | 要件 |
|---|---|
| OS | Windows 10/11, Linux (Ubuntu 22.04+) |
| GPU | Vulkan 1.2 以上対応 (離散GPU推奨) |
| VRAM | 4 GB 以上 |
| RAM | 16 GB 以上 |
| CPU | x86_64, 4コア以上, SSE2 必須 |

### 2.3 言語・ツールチェーン

| 項目 | 選定 | 理由 |
|---|---|---|
| 言語 | C++ (C++20) | Vulkan API との親和性、最大性能 |
| ビルドシステム | CMake 3.24+ | Win/Linux クロスプラットフォーム |
| Vulkan SDK | LunarG Vulkan SDK 1.3+ | バリデーションレイヤー、プロファイラ含む |
| ウィンドウ | GLFW 3.3+ | 軽量、Vulkan surface 生成対応 |
| UI | Dear ImGui | HUD オーバーレイ、デバッグ表示 |
| シェーダ | GLSL → SPIR-V (glslc) | 標準ツールチェーン |
| プロファイリング | CPU chrono + Vulkan fence 計測 | パイプライン各段の計測 |

---

## 3. アーキテクチャ概要

### 3.1 パイプライン全体像

```
┌──────────────────────────────┐     Anonymous Pipe (stdout/stdin)     ┌───────────────────────────────────┐
│        grebe-sg プロセス      │ ─────────────────────────────────────▶ │            grebe プロセス          │
│     (Signal Generator UI)    │      FrameHeaderV2 + payload          │        (Visualization UI)         │
│                              │ ◀───────────────────────────────────── │                                   │
│  [SG UI]                     │          IpcCommand (control)          │  [IPC 受信]                        │
│   ├─ global sample rate      │                                       │   ├─ pipe read → local ring buffer│
│   └─ per-ch waveform/length  │                                       │   └─ sequence/timestamp validation│
│                              │                                       │  [間引きスレッド]                  │
│  [データ生成スレッド]          │                                       │   └─ MinMax/LTTB                  │
│   └─ push frame blocks       │                                       │  [描画スレッド (メイン)]            │
│      to transport producer   │                                       │   └─ Vulkan render + HUD          │
└──────────────────────────────┘                                       └───────────────────────────────────┘
```

**注:** `--embedded` モードでは grebe-sg を起動せず、grebe プロセス内で DataGenerator を直接実行する。

### 3.2 コンポーネント構成

```
grebe/
├── src/
│   ├── main.cpp                  # grebe エントリポイント (SG 起動 + IPC 受信 + app_loop)
│   ├── sg_main.cpp               # grebe-sg エントリポイント (SG UI + sender thread)
│   ├── app_loop.*                # メインループ (Vulkan render + profiler + HUD)
│   ├── cli.*                     # CLI オプション解析
│   ├── vulkan_context.*          # Vulkan 初期化・デバイス管理
│   ├── swapchain.*               # スワップチェーン管理
│   ├── renderer.*                # 描画パイプライン
│   ├── buffer_manager.*          # Triple-buffered upload
│   ├── decimator.*               # 間引きアルゴリズム (MinMax SIMD / LTTB)
│   ├── decimation_thread.*       # 間引きワーカースレッド (マルチスレッド対応)
│   ├── data_generator.*          # 合成データ生成
│   ├── benchmark.*               # テレメトリ収集
│   ├── profiler.*                # 自動プロファイリング (envelope 検証含む)
│   ├── envelope_verifier.*       # 波形整合性検証 (sliding-window min/max)
│   ├── hud.*                     # Main UI/HUD
│   ├── microbench.*              # 独立マイクロベンチマーク
│   ├── compute_decimator.*       # GPU Compute Shader 間引き (実験)
│   ├── ring_buffer.h             # Lock-free SPSC ring buffer
│   ├── ring_buffer_view.h        # Raw pointer 抽象化
│   ├── drop_counter.h            # Per-channel drop/backpressure 計測
│   ├── app_command.*             # Command DTO + AppCommandQueue
│   ├── process_handle.*          # Cross-platform プロセス管理
│   └── ipc/                      # プロセス間通信
│       ├── contracts.h           # FrameHeaderV2, IpcCommand
│       ├── transport.h           # ITransportProducer / ITransportConsumer 抽象 I/F
│       ├── pipe_transport.*      # Anonymous pipe 実装
│       └── (shm_transport.*)     # Shared memory 実装 (延期マイルストーン)
├── shaders/
├── doc/
└── scripts/
```

### 3.3 IPC プロトコル仕様

#### 現行実装: Anonymous Pipe Transport

`grebe` は `grebe-sg` を子プロセスとして起動し、stdin/stdout を anonymous pipe で接続する。

- **Data (SG → Main):** `FrameHeaderV2` + channel-major payload を stdout 経由で送出
- **Control (Main → SG):** `IpcCommand` を stdin 経由で送信（SET_SAMPLE_RATE / TOGGLE_PAUSED / QUIT）
- **Transport 抽象 I/F:** `ITransportProducer` / `ITransportConsumer` (`src/ipc/transport.h`) で実装を切り離し可能

#### 3.3.1 FrameHeaderV2 (実装準拠)

```c
struct FrameHeaderV2 {
    uint32_t magic;                 // 'GFH2' (0x32484647)
    uint32_t header_bytes;          // = sizeof(FrameHeaderV2)
    uint64_t sequence;              // 単調増加
    uint64_t producer_ts_ns;
    uint32_t channel_count;
    uint32_t block_length_samples;  // samples per channel
    uint32_t payload_bytes;         // = channel_count * block_length_samples * sizeof(int16_t)
    uint32_t header_crc32c;         // プレースホルダ (Phase 13 で実装予定)
    double   sample_rate_hz;        // SG 権限のレート (grebe-sg → grebe 自動同期)
    uint64_t sg_drops_total;        // SG 側累積ドロップ
    uint64_t first_sample_index;    // 各チャンネルの先頭サンプル絶対インデックス
    // payload: [ch0][ch1]...[chN-1] (channel-major, int16_t)
};
```

#### 3.3.2 IpcCommand (Main → SG)

```c
struct IpcCommand {
    enum Type : uint32_t {
        SET_SAMPLE_RATE = 1,
        TOGGLE_PAUSED   = 2,
        QUIT            = 3,
    };
    uint32_t magic;   // 'GIC2' (0x32434947)
    uint32_t type;
    double   value;   // SET_SAMPLE_RATE 時のみ使用
};
```

既定値:
- `block_length_samples = 16384` (CLI: `--block-size`, 1024〜65536)
- `sequence gap 許容 = 0` (gap は drop として記録)

#### 延期マイルストーン: Shared Memory IPC 設計メモ

> 以下は TI-08 で延期判定された Shared Memory IPC の設計メモである。
> ボトルネックが pipe transport ではなく消費側にあること、および Embedded モードで 0-drops が達成済みであることから、PoC では anonymous pipe で十分と判断された。
> 再開トリガーは TODO.md「延期マイルストーン」を参照。

<details>
<summary>Shared Memory IPC 仕様 (未実装)</summary>

**共有メモリオブジェクト:**

| 名称 | 既定名 | 用途 | 読み書き |
|---|---|---|---|
| ControlBlockV2 | `grebe-ipc-ctrl` | discovery / config / queue descriptor / producer heartbeat | SG 書き込み, Main 読み取り |
| ConsumerStatusBlockV2 | `grebe-ipc-cons` | credit window / consumer heartbeat / read progress | Main 書き込み, SG 読み取り |
| DataRingV2 | `grebe-ipc-data-<generation>` | フレームデータ本体リング | SG 書き込み, Main 読み取り |

**制御ブロック/状態ブロック:**

```c
struct SignalConfigV2 {
    uint32_t version;               // = 2
    uint32_t channel_count;         // 1..8
    double   global_sample_rate_hz;
    uint32_t block_length_samples;  // 全ch共通固定長
    struct {
        uint8_t enabled;
        uint8_t waveform;
        uint16_t reserved;
    } channels[8];
};

struct DataRingDescV2 {
    uint32_t slot_count;            // 既定 64
    uint32_t slot_stride_bytes;     // header + payload + alignment
    uint32_t max_payload_bytes;
    uint32_t feature_flags;         // bit0=doorbell_event, bit1=payload_crc32c
};

struct ControlBlockV2 {
    uint32_t magic;                 // 'GRB2'
    uint32_t abi_version;           // = 2
    uint32_t endianness;            // 0=little
    uint32_t reserved0;
    uint64_t generation;
    uint64_t producer_heartbeat_ns;
    char     data_segment_name[64];
    DataRingDescV2 ring_desc;
    SignalConfigV2 active_config;
    uint32_t status_flags;          // bit0=ready, bit1=degraded
    uint32_t reserved1;
};

struct ConsumerStatusBlockV2 {
    uint32_t magic;                 // 'GCS2'
    uint32_t version;               // = 2
    uint64_t observed_generation;
    uint64_t consumer_heartbeat_ns;
    uint64_t read_sequence;         // 取り込み完了済みseq
    uint32_t credits_granted;       // producer送信可能枠
    uint32_t last_error_code;
    uint64_t drops_observed;
};
```

**フロー制御:** credit-based window (drop-new policy)。**ハンドシェイク:** generation-based attach/reattach。詳細は v1.5.0 以前の本文書を参照。

</details>

### 3.4 標準I/Fとの対応

| 観点 | 現行 (pipe) | 将来 (shm) | 参照パターン |
|---|---|---|---|
| データ搬送 | anonymous pipe (stdin/stdout) | 固定長 descriptor-like ring | AF_XDP/DPDK |
| 制御/データ分離 | IpcCommand 別チャネル | ControlBlock + DataRing 分離 | Aeron |
| フロー制御 | pipe backpressure (OS) | credit window | PCIe/NVMe queue depth |
| 通知 | blocking read | poll + optional doorbell | NVMe/xHCI |
| 信頼性 | loss-tolerant (SG drops 計測) | loss-tolerant + drop accounting | UDP realtime profile |

### 3.5 同期整合性とトリガベース捕捉（SG/Main 両側）

表示波形が入力信号を忠実に再現していることを保証するため、
`grebe-sg` と `grebe` の双方で同期・検証機構を実装する。

#### PoC 実装済み（Phase 11/11c）

- SG 側:
  - periodic timer による固定周期 capture window でデータを送出する
  - 各フレームに capture window 境界（`first_sample_index`、`sg_drops_total`）を付与する
- Main 側:
  - sequence continuity を検証し、フレーム欠落を検知する（IPC: FrameHeaderV2.sequence、Embedded: DropCounter）
  - window coverage（capture window 充足率）を品質指標として継続監視する（HUD + `--profile` JSON）
  - 既知合成信号に対する envelope 検証を `--profile` に統合する（Phase 11c: lazy-caching verifier, Embedded 全レート 100%）

#### 製品拡張（Phase 15+ scope）

- SG 側:
  - internal trigger（level/edge, pre/post trigger window）を追加する
  - external trigger 入力パスを提供し、フォールバックとして timer を利用可能にする
- Main 側:
  - trigger-aligned frame validity 判定を実装する
  - Embedded 基準との同期比較フレームワークを構築する

---

## 4. 機能要件

### FR-01: 合成データ生成

- **FR-01.1:** 指定サンプリングレート（最大 1 GSPS）で int16 データを連続生成する
- **FR-01.2:** 波形パターンとして以下を選択可能にする
  - 正弦波（周波数可変）
  - 矩形波
  - ノコギリ波
  - ホワイトノイズ
  - チャープ信号（周波数掃引）
- **FR-01.3:** 生成は専用スレッドで実行し、ring buffer に書き込む
- **FR-01.4:** 生成レートを段階的に変更可能にする（1MSPS / 10MSPS / 100MSPS / 1GSPS）
- **FR-01.5:** ≥100 MSPS では period tiling (memcpy) による高速生成を行う

### FR-02: データ間引き

- **FR-02.1:** MinMax decimation を実装する（各ピクセル幅に対して min/max ペアを出力、SSE2 SIMD 最適化）
- **FR-02.2:** LTTB (Largest Triangle Three Buckets) を実装する
- **FR-02.3:** 間引き無し（raw データ直接描画）モードを提供する（低レートベンチマーク用）
- **FR-02.4:** 間引き率 = (入力サンプル数) / (出力サンプル数) を動的に変更可能にする
- **FR-02.5:** 間引き処理は描画スレッドとは別スレッドで実行する
- **FR-02.6:** ≥100 MSPS では LTTB を自動無効化し、MinMax にフォールバックする

### FR-03: Vulkan 描画パイプライン

- **FR-03.1:** int16 データを頂点バッファとして GPU に転送し、Line Strip として描画する
- **FR-03.2:** Staging Buffer → Device Local Buffer の非同期転送を行う
- **FR-03.3:** Triple Buffering: CPU 書き込み用 / 転送中 / GPU 描画中の3面を管理する
- **FR-03.4:** Vertex Shader でデータ変換を行う（int16 → NDC 座標へのスケーリング）
- **FR-03.5:** ビューポート変換パラメータ（時間軸スケール、振幅スケール、オフセット）を Push Constants で渡す
- **FR-03.6:** V-Sync ON/OFF を切り替え可能にする（OFF 時の最大描画レート計測用）

### FR-04: ウィンドウ・表示

- **FR-04.1:** GLFW ウィンドウに波形を描画する（デフォルト: 1920×1080）
- **FR-04.2:** ウィンドウリサイズ対応（スワップチェーン再生成）
- **FR-04.3:** 画面上にリアルタイムメトリクスを ImGui HUD 表示する（FR-06 参照）

### FR-05: ベンチマークモード

- **FR-05.1:** 自動プロファイリングシーケンス (`--profile`) を実行可能にする
  - 4シナリオ (1M/10M/100M/1G SPS) × 各 120 フレームウォームアップ + 300 フレーム計測
  - JSON レポート出力、pass/fail 判定（≥30 FPS + envelope 一致率）
  - 各シナリオで既知合成信号の MinMax 出力を理論 envelope と比較し、一致率を計測する
- **FR-05.2:** ベンチマーク結果を CSV/JSON ファイルに出力する
- **FR-05.3:** 以下の独立したマイクロベンチマーク (`--bench`) を提供する
  - **BM-A:** CPU→GPU 転送スループット（vkCmdCopyBuffer, 1/4/16/64 MB）
  - **BM-B:** 間引きスループット（MinMax scalar / MinMax SIMD / LTTB, 16M samples）
  - **BM-C:** 描画スループット（V-Sync OFF, 3.8K/38.4K/384K 頂点）
  - **BM-E:** GPU Compute Shader 間引きスループット

### FR-06: メトリクス収集・表示

以下のメトリクスをリアルタイムで計測・HUD 表示する:

| メトリクス | 単位 | 説明 |
|---|---|---|
| FPS | frames/sec | 描画フレームレート |
| Frame Time | ms | 1フレームの所要時間 |
| Input Data Rate | MSPS / GSPS | 入力データレート |
| Ring Buffer Fill | % | ring buffer の充填率 |
| Vertex Count | count | 1フレームの頂点数 |
| Drain Time | ms | ring buffer 読み出し時間 |
| Decimation Time | ms | 間引き処理時間 |
| Decimation Ratio | x:1 | 間引き率 |
| Upload Time | ms | GPU バッファ転送時間 |
| Swap Time | ms | バッファ切替時間 |
| Render Time | ms | 描画実行時間 |
| Samples/Frame | count | フレームあたりの入力サンプル数 |
| Window Coverage | % | capture window 充足率 |
| Seq Gaps | count | IPC sequence gap 検知数 |
| Viewer Drops | count | viewer 側リングバッファドロップ累計 |
| SG Drops | count | SG 側で発生した累積ドロップ |
| Envelope Match | % | 既知信号 envelope 一致率 (`--profile` 時) |
| Trigger Mode | enum | internal / external / timer — **Phase 15+ scope** |
| Trigger Lock | bool | trigger成立状態 — **Phase 15+ scope** |
| Frame Validity | enum / % | valid/invalid と valid frame 比率 — **Phase 15+ scope** |

### FR-07: マルチチャンネル表示

- **FR-07.1:** `--channels=N` (N=1,2,4,8) で起動時にチャンネル数を指定可能にする（デフォルト: 1）
- **FR-07.2:** 各チャンネルを画面内の独立した垂直レーンに描画する
- **FR-07.3:** 各チャンネルに固有の色を割り当てる（Ch0=緑, Ch1=黄, Ch2=シアン, Ch3=マゼンタ 等）
- **FR-07.4:** 各チャンネルで独立に間引き処理を行う
- **FR-07.5:** N=1 のとき従来と同一の動作を保証する（後方互換）

### FR-08: プロセス分離と起動制御

- **FR-08.1:** 可視化メインを `grebe`、信号生成を `grebe-sg` の別実行バイナリとして提供する
- **FR-08.2:** `grebe` はデフォルトで `grebe-sg` を子プロセスとして自動起動する
- **FR-08.3:** 既存 `grebe-sg` へ接続する attach モード (`--attach-sg`) — **将来拡張** (shm 実装が前提)
- **FR-08.4:** `grebe-sg` の停止検知と復帰
  - child モード: pipe EOF 検知
  - attach モード/heartbeat 監視 — **将来拡張** (shm scope)
- **FR-08.5:** 可視化UIに信号生成設定UIを持ち込まず、責務分離を維持する
- **FR-08.6:** `grebe` + `grebe-sg` の E2E 動作を必須とする
- **FR-08.7:** `--embedded` フラグで in-process DataGenerator モードを提供する

### FR-09: SG UI とチャンネル設定モデル

- **FR-09.1:** SG UI は専用ウィンドウで動作する（headless fallback 付き）
- **FR-09.1a:** SG UI 描画バックエンドは `GLFW + OpenGL + ImGui` を採用する
- **FR-09.2:** チャンネル数は 1-8 をサポートする
- **FR-09.3:** サンプルレートはグローバル設定（全チャンネル共通）とする（`FrameHeaderV2.sample_rate_hz` で grebe に自動同期）
- **FR-09.4:** 各チャンネルごとに modulation/waveform を独立設定可能にする
- **FR-09.5:** data length (block length) は全チャンネル共通値とする（固定長フレーミング、`--block-size` で指定）
- **FR-09.6:** 各チャンネル独立の可変 data length は拡張要件として後続フェーズで扱う
- **FR-09.7:** SG 側に trigger mode 設定を追加する（internal / external / periodic timer） — **Phase 15+ scope**
- **FR-09.8:** internal trigger は level/edge（rising/falling/both）を設定可能にする — **Phase 15+ scope**
- **FR-09.9:** SG 側は trigger をサンプル生成クロックで判定し、capture window 境界を IPC で通知する — **Phase 15+ scope**
- **FR-09.10:** external trigger 未入力時は periodic timer へフォールバック可能にする — **Phase 15+ scope**

### FR-10: トランスポート抽象化と計測

- **FR-10.1:** `grebe-sg` → `grebe` のデータ搬送は抽象 I/F 経由で実装する（`ITransportProducer`/`ITransportConsumer`）
- **FR-10.2:** 初期実装は anonymous pipe とする（Shared memory は延期マイルストーン）
- **FR-10.3:** 外部インターフェース帯域/遅延評価のため、代替バックエンドを差し替え可能にする
- **FR-10.4:** attach/discovery 用 `ControlBlockV2` — **延期** (shm 実装時に対応)
- **FR-10.5:** consumer 状態共有用 `ConsumerStatusBlockV2` — **延期** (shm 実装時に対応)
- **FR-10.6:** credit-based window フロー制御 — **延期** (現行 pipe は OS backpressure に依存)
- **FR-10.7:** 信頼性プロファイルは loss-tolerant realtime とし、SG 側 drops を計測・記録する
- **FR-10.8:** `FrameHeaderV2.header_crc32c` 検証 — **Phase 13 scope** (現在プレースホルダ)
- **FR-10.9:** transport 指標計測（drop rate, samples/frame, SG drops）。inflight depth/credits は **延期** (shm scope)
- **FR-10.10:** E2E timestamp delta — **Phase 12 scope**
- **FR-10.11:** config 更新は `IpcCommand` 経由のみ許可する
- **FR-10.12:** trigger/capture 境界メタデータ — **Phase 15+ scope**

### FR-11: フレーム有効性判定と波形整合性

- **FR-11.1:** Main 側で frame validity 判定を必須化する（valid / invalid）
  - sequence gap + drop 検知で実装。CRC ベースの per-frame valid/invalid 判定は **Phase 13+ scope**
- **FR-11.2:** validity 判定条件として以下を最低限含める
  - sequence continuity（欠落検知）
  - CRC 検証（header/payload） — **Phase 13 scope** (header_crc32c はプレースホルダ)
  - capture window 充足率
- **FR-11.3:** invalid frame は HUD/ログ/プロファイルで明示し、silent success を禁止する
  - drops/seq_gaps は HUD 表示。per-frame valid/invalid フラグは **Phase 13+ scope**
- **FR-11.4:** 波形整合性メトリクスを段階的に導入する
  - **PoC tier（Phase 11）:** window coverage ratio、既知信号 envelope 検証（MinMax 出力 vs 理論 envelope、±1 LSB 許容）
  - **PoC tier 改善（Phase 11c）:** lazy-caching envelope verifier。実フレームの `ch_raw` から bucket size を算出し、per-bucket-size で valid pair set を on-demand build + cache。高レート (1 GSPS) × 多チャンネル (4ch) で envelope 一致率 100% 達成
  - **Product tier（将来）:** Embedded/IPC 比較 envelope mismatch rate、peak miss rate、extremum amplitude error（p50/p95/p99）
- **FR-11.5:** viewer drops と SG drops を別系統で記録し、品質判定に反映する

---

## 5. 非機能要件

### NFR-01: 性能目標

| レベル | 入力レート | 間引き | 描画FPS | 判定 | 結果 |
|---|---|---|---|---|---|
| L0: 基本動作 | 1 MSPS | MinMax | 60 fps | 必須 | **PASS** (60.0 fps) |
| L1: 中速 | 100 MSPS | MinMax | 60 fps | 必須 | **PASS** (59.9 fps) |
| L2: 高速 | 1 GSPS | MinMax | 60 fps | 目標 | **PASS** (59.7 fps) |
| L3: 限界探索 | 1 GSPS | MinMax | V-Sync OFF 最大 | 計測のみ | **2,022 FPS** |

計測環境: RTX 5080, NVIDIA ネイティブ Vulkan, Windows, MSVC Release

### NFR-01b: マルチチャンネル性能目標

| レベル | チャンネル数 | 入力レート/ch | 間引き | 描画FPS | 判定 | 結果 |
|---|---|---|---|---|---|---|
| MC-1 | 4 | 1 MSPS | MinMax | ≥30 fps | 必須 | **PASS** (60.1 fps) |
| MC-2 | 4 | 100 MSPS | MinMax | ≥30 fps | 目標 | **PASS** (60.0 fps) |
| MC-3 | 8 | 10 MSPS | MinMax | ≥30 fps | 目標 | **PASS** (60.0 fps) |

### NFR-02: レイテンシ

- データ生成からピクセル表示までの E2E レイテンシを計測する
- 目標: L1 で 50ms 以下、L2 で 100ms 以下
- 推定: ring buffer + ダブルバッファ + V-Sync の 3 フレーム分 ≈ 50ms

### NFR-02b: 波形表示整合性

- 既知合成信号に対する envelope 一致検証を `--profile` で自動実行する
  - Embedded モード: envelope 一致率 100%（±1 LSB）を全レート・全チャンネル構成で必須とする
  - 高レート (≥100 MSPS) では lazy-caching verifier（実フレーム ch_raw から bucket size を算出、per-bucket-size on-demand build + cache）で検証精度を保証する
  - IPC モード: SG-side drops に応じた乖離を定量計測し、TI-10 で評価する
- sequence continuity（フレーム欠落なし）を検証する
  - Embedded モード: 欠落 0 を必須とする
  - IPC モード: 欠落率を計測し記録する
- periodic timer モードでは window coverage の下限しきい値を定義する
  - Embedded モード: 95% 以上を目標
  - IPC モード: SG-side drops の影響により低下する（TI-09: 4ch×1G で ~63%）。モード別に閾値を設定し、pipe 帯域制約下での許容範囲を明記する
- PoC tier の品質判定は FPS/頂点数 + envelope 一致率 + valid frame ratio + window coverage ratio で行う。Product tier の整合性メトリクス（FR-11.4 product tier）は製品化フェーズで追加する

### NFR-03: メモリ使用量

- Ring buffer サイズ: デフォルト 64M samples (128 MB)、`--ring-size` で変更可能
- GPU Staging Buffer: 3面 × 描画頂点数分（数十KB程度）
- VRAM 総使用量: 512 MB 以下
- IPC DataRing (v2) は `block_length_samples=65536`, `slot_count=64` を基準とし、
  8ch 時の payload 領域目安を約 64 MB に制約する（header/管理領域を除く）

### NFR-04: 安定性

- 1時間連続実行でクラッシュ・メモリリーク・VRAM リークが発生しないこと
- Vulkan Validation Layer エラーが0件であること

### NFR-05: ビルド・ポータビリティ

- Windows (MSVC 2022+) と Linux (GCC 12+ / Clang 15+) でビルド可能
- 外部依存は CMake FetchContent で解決
- GPU ベンダー非依存（NVIDIA / AMD / Intel で動作）

### NFR-06: IPC 安定性

- `grebe-sg` の異常終了時、`grebe` はクラッシュせず pipe EOF を検知して停止する
- `grebe-sg` の再起動後の自動再接続 — **将来拡張**
- 1時間連続実行でリーク/ハングがないこと — **Phase 13 scope** (未検証)
- sequence 欠落を検知できること
- credit window 制御下で producer/consumer がデッドロックしないこと — **延期** (shm scope)
- header CRC 異常を確実に検出し、破損フレームを破棄できること — **Phase 13 scope**

---

## 6. 技術的検証項目

PoCを通じて以下の技術的疑問に回答を得た。詳細は `doc/technical_judgment.md` を参照。

| TI | 項目 | 結論 |
|---|---|---|
| TI-01 | GPU 転送ボトルネック | 間引き後 7.68 KB/frame — **ボトルネックではない** (3環境で検証済み) |
| TI-02 | CPU 間引き性能 | MinMax SIMD ~20 GSPS (1 GSPS に 20x マージン)。**LTTB は ≥100 MSPS で不適** |
| TI-03 | GPU 側間引き | CPU SIMD が 3.9x 高速。**CPU 側間引き + 小データ転送が最適** |
| TI-04 | 描画プリミティブ | LINE_STRIP 3840 vtx → 2,022 FPS (34x 余裕)。**十分** |
| TI-05 | 永続マップドバッファ | **検証不可** (dzn は ReBAR 非対応)。現行設計で優先度低 |
| TI-06 | スレッドモデル | 3 スレッド + lock-free SPSC が最適。ring_fill <0.3% @ 1 GSPS |
| TI-07 | IPC パイプ帯域と欠落率 | Windows ネイティブ ~100-470 MB/s (最適化後)。shm 延期判定 |
| TI-08 | IPC ボトルネック再評価 | ボトルネックは pipe ではなくパイプライン (cache cold + ring drain)。マルチスレッド間引きで 0-drops 達成 |
| TI-09 | SG-side drop 評価 | IPC 4ch×1G で SG drops ~37%。可視化品質に影響なし (MinMax 3840 vtx/ch 不変)。**PoC 許容** |
| TI-10 | 波形表示整合性検証 | Embedded 全レート envelope 100%。lazy-caching verifier で高レート対応。**検証完了** |

---

## 7. 操作仕様

`grebe` (可視化) と `grebe-sg` (信号生成) の 2 プロセス構成を前提とする。

### キーボード操作

| キー | 操作 |
|---|---|
| `1` - `4` | `grebe-sg`: グローバルサンプルレート切替 (1M / 10M / 100M / 1G SPS) |
| `D` | `grebe`: 間引きアルゴリズム切替 (None / MinMax / LTTB) |
| `V` | `grebe`: V-Sync ON/OFF トグル |
| `Space` | `grebe`: 一時停止 / 再開コマンド送信 |
| `Esc` | フォーカス中プロセスを終了 |

### CLI オプション (`grebe`)

| オプション | 説明 | 実装状況 |
|---|---|---|
| `--log` | CSV テレメトリを `./tmp/` に出力 | 実装済み |
| `--profile` | 自動プロファイリング実行、JSON レポート出力 | 実装済み |
| `--bench` | 独立マイクロベンチマーク実行、JSON 出力 | 実装済み |
| `--embedded` | grebe-sg を起動せず in-process DataGenerator を使用 | 実装済み |
| `--ring-size=<N>[K\|M\|G]` | Ring buffer サイズ指定（デフォルト 64M） | 実装済み |
| `--channels=N` | チャンネル数指定（デフォルト 1, 最大 8） | 実装済み |
| `--block-size=N` | IPC ブロックサイズ (1024〜65536, 2冪, デフォルト 16384) | 実装済み |
| `--no-vsync` | V-Sync 無効化 | 実装済み |
| `--attach-sg` | `grebe-sg` を自動起動せず既存プロセスへ接続 | 将来拡張 |
| `--sg-path=<path>` | 自動起動時に使用する `grebe-sg` パス | 将来拡張 |
| `--transport=<name>` | トランスポート実装選択 | 将来拡張 |
| `--trigger-mode=...` | 捕捉モード指定 | Phase 15+ |

### CLI オプション (`grebe-sg`)

| オプション | 説明 | 実装状況 |
|---|---|---|
| `--channels=N` | チャンネル数指定（デフォルト 1, 最大 8） | 実装済み |
| `--sample-rate=<Hz>` | グローバルサンプルレート初期値 | 実装済み |
| `--ring-size=<N>[K\|M\|G]` | Ring buffer サイズ指定 | 実装済み |
| `--block-size=N` | IPC ブロックサイズ | 実装済み |
| `--transport=<name>` | トランスポート実装選択 | 将来拡張 |
| `--trigger-*` | Trigger 関連オプション | Phase 15+ |

---

## 8. 成果物

| 成果物 | 形式 | 説明 |
|---|---|---|
| ソースコード | C++ / CMake | ビルド可能なPoC一式 |
| プロファイルレポート | JSON | 4シナリオの自動計測結果 |
| マイクロベンチマーク結果 | JSON | BM-A/B/C/E の計測値 |
| テレメトリログ | CSV | フレーム単位の詳細計測値 |
| 技術的判断メモ | Markdown | TI-01〜10 への回答と推奨事項 |

---

## 9. リスクと制約

| リスク | 影響 | 結果 |
|---|---|---|
| 1GSPS の CPU 間引きが追いつかない | L2 未達 | **緩和済み**: MinMax SIMD ~20 GSPS で 20x マージン |
| PCIe 帯域が律速 | L2 未達 | **設計で回避**: 間引き後 7.68 KB/frame のみ転送 |
| Vulkan 初期化の複雑さ | 全体遅延 | **緩和済み**: vk-bootstrap 活用 |
| GPU ベンダー間の挙動差 | 再現性低下 | **検証済み**: RTX 5080 で複数環境動作確認 |
| LTTB が高レートで追いつかない | 描画落ち | **対策済み**: ≥100 MSPS で自動無効化 |
| 親子プロセス管理の複雑化 | 起動失敗/孤児プロセス | **緩和済み**: spawn_with_pipes + pipe EOF 検知。attach モードは将来拡張 |
| IPC 同期不整合（seq 欠落、再接続） | データ欠損/停止 | **緩和済み**: sequence continuity 検証 + drop 計測 (TI-09/10) |
| トリガ未ロック/誤トリガ | 無効波形表示 | **Phase 15+ scope**: 現行は periodic timer のみ |
| Pipe IPC 帯域上限 | 高レート SG drops | **PoC 許容**: SG drops は可視化品質に影響なし (TI-09)。shm は延期 |

---

## 付録A: データ量試算

| レート | 1フレーム分 (@ 60fps) | 1秒分 | Ring Buffer (1秒) |
|---|---|---|---|
| 1 MSPS | 16,667 samples = 33 KB | 2 MB | 2 MB |
| 10 MSPS | 166,667 samples = 333 KB | 20 MB | 20 MB |
| 100 MSPS | 1,666,667 samples = 3.3 MB | 200 MB | 200 MB |
| 1 GSPS | 16,666,667 samples = 33 MB | 2 GB | 2 GB |

**描画側（MinMax 間引き後）:**
画面幅 1920px の場合、MinMax で 1920 × 2 = 3840 頂点/フレーム → **約 7.68 KB/フレーム**。
間引き後の GPU 転送量は全レートで事実上同一であり、パイプライン設計の正しさが計測で確認された。

## 付録B: 依存ライブラリ

| ライブラリ | 用途 | ライセンス | 導入方法 |
|---|---|---|---|
| GLFW | ウィンドウ・入力 | Zlib | FetchContent |
| vk-bootstrap | Vulkan 初期化簡略化 | MIT | FetchContent |
| VMA (Vulkan Memory Allocator) | メモリ管理 | MIT | FetchContent |
| glm | 数学（行列・ベクトル） | MIT | FetchContent |
| Dear ImGui | HUD オーバーレイ | MIT | FetchContent |
| stb_truetype | テキスト描画（ImGui 内部） | Public Domain | FetchContent (ImGui同梱) |
| nlohmann/json | ベンチマーク結果出力 | MIT | FetchContent |
| spdlog | ロギング | MIT | FetchContent |

## 付録C: 計測結果サマリ

計測環境: RTX 5080, NVIDIA ネイティブ Vulkan, Windows, MSVC Release, Ryzen 9 9950X3D

### マイクロベンチマーク

| ベンチマーク | 結果 |
|---|---|
| BM-A: CPU→GPU 転送 | 10.3 (1MB) / 18.3 (4MB) / 22.2 (16MB) / 23.7 (64MB) GB/s |
| BM-B: MinMax Scalar | 1,884 MSPS |
| BM-B: MinMax SIMD (SSE2) | 19,834 MSPS |
| BM-B: LTTB | 743 MSPS |
| BM-B: SIMD 高速化率 | **10.5x** |
| BM-C: 描画 (3840 vtx) | **2,022 FPS** (0.49 ms/frame) |
| BM-C: 描画 (38400 vtx) | **3,909 FPS** (0.26 ms/frame) |
| BM-C: 描画 (384000 vtx) | **3,741 FPS** (0.27 ms/frame) |
| BM-E: GPU Compute MinMax | **5,127 MSPS** (3.2 ms/call) |

### プロファイル結果 (1ch)

| シナリオ | FPS avg | FPS min | Frame ms | Render ms |
|---|---|---|---|---|
| 1MSPS | 60.3 | 48.1 | 16.64 | 11.00 |
| 10MSPS | 60.2 | 48.9 | 16.64 | 11.07 |
| 100MSPS | 60.2 | 52.3 | 16.63 | 10.95 |
| 1GSPS | 60.2 | 52.8 | 16.62 | 11.11 |

詳細は `doc/technical_judgment.md` を参照。
