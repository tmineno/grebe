# Grebe — Vulkan 高速時系列ストリーム描画 PoC/MVP 要件定義書

**バージョン:** 1.4.0
**最終更新:** 2026-02-08

---

## 1. 目的と背景

### 1.1 目的

Vulkan を用いた時系列データストリームの高速描画パイプラインの技術的限界を検証する PoC/MVP を構築する。最終的なゴールはリアルタイム信号可視化基盤の実現可能性評価であり、本PoCではパイプライン各段のスループット・レイテンシを定量的に計測し、ボトルネックを特定する。

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

**本PoCに含まないもの:**
- 実デバイス（ADC/FPGA）との接続
- 保存・再生機能
- 本格的な UI/UX（メニュー、設定画面等）
- ネットワーク経由のデータ受信

**次期マイルストーン（本書で要件化、実装はこれから）:**
- 実行バイナリの分離: `grebe` (可視化メイン) / `grebe-sg` (信号生成)
- `grebe` から `grebe-sg` の自動起動（デフォルト）と attach モード
- SG 専用 UI（チャンネル設定）と Main 可視化 UI の責務分離
- Shared Memory (`memcpy`) ベース IPC と外部I/F評価向けトランスポート抽象化
- フェーズ境界ごとに必ず runnable な 2 プロセス系を維持（分離だけで通信不能な中間状態を作らない）

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
┌──────────────────────────────┐          Control + Data (IPC)         ┌───────────────────────────────────┐
│        grebe-sg プロセス      │ ─────────────────────────────────────▶ │            grebe プロセス          │
│     (Signal Generator UI)    │                                       │        (Visualization UI)         │
│                              │ ◀───────────────────────────────────── │                                   │
│  [SG UI]                     │          Status / backpressure         │  [IPC 受信]                        │
│   ├─ global sample rate      │                                       │   ├─ Shared memory dequeue        │
│   └─ per-ch waveform/length  │                                       │   └─ sequence/timestamp validation│
│                              │                                       │  [間引きスレッド]                  │
│  [データ生成スレッド]          │                                       │   └─ MinMax/LTTB                  │
│   └─ push frame blocks       │                                       │  [描画スレッド (メイン)]            │
│      to transport producer   │                                       │   └─ Vulkan render + HUD          │
└──────────────────────────────┘                                       └───────────────────────────────────┘
```

### 3.2 コンポーネント構成

```
grebe/
├── src/
│   ├── app_grebe/                # 可視化メイン実行バイナリ (grebe)
│   │   ├── main.cpp              # エントリポイント、SG起動/attach制御
│   │   ├── vulkan_context.*      # Vulkan 初期化・デバイス管理
│   │   ├── swapchain.*           # スワップチェーン管理
│   │   ├── renderer.*            # 描画パイプライン
│   │   ├── buffer_manager.*      # Triple-buffered upload
│   │   ├── decimator.*           # 間引きアルゴリズム
│   │   ├── decimation_thread.*   # 間引きワーカースレッド
│   │   ├── benchmark.*           # テレメトリ収集
│   │   └── hud.*                 # Main UI/HUD
│   ├── app_grebe_sg/             # 信号生成実行バイナリ (grebe-sg)
│   │   ├── main.cpp              # SG UI + 設定適用 (GLFW + OpenGL + ImGui)
│   │   └── data_generator.*      # 合成データ生成
│   ├── ipc/                      # プロセス間通信契約/実装
│   │   ├── contracts.h           # SignalConfigV2, FrameHeaderV2 等
│   │   ├── transport.h           # Producer/Consumer 抽象I/F
│   │   └── shm_transport.*       # Shared memory (memcpy) 実装
│   └── common/                   # 共有ユーティリティ (ring_buffer.h, types.h, time_utils.h など)
├── shaders/
├── doc/
└── scripts/
```

**実装方針メモ:**
- `src/` の大規模ディレクトリ移動は高リスクのため、ロジック変更と分離せず独立コミットで実施する
- フェーズ初期は既存配置を維持したまま 2 プロセス化を優先し、段階的に再配置してよい

### 3.3 IPC プロトコル仕様 (v2)

`grebe-sg` (producer) と `grebe` (consumer) は Shared Memory 上の
**ControlBlockV2 + ConsumerStatusBlockV2 + DataRingV2** で通信する。
本仕様は単一バージョン運用とし、後方互換は考慮しない。

#### 3.3.1 共有メモリオブジェクト

| 名称 | 既定名 | 用途 | 読み書き |
|---|---|---|---|
| ControlBlockV2 | `grebe-ipc-ctrl` | discovery / config / queue descriptor / producer heartbeat | SG 書き込み, Main 読み取り |
| ConsumerStatusBlockV2 | `grebe-ipc-cons` | credit window / consumer heartbeat / read progress | Main 書き込み, SG 読み取り |
| DataRingV2 | `grebe-ipc-data-<generation>` | フレームデータ本体リング | SG 書き込み, Main 読み取り |

#### 3.3.2 制御ブロック/状態ブロック

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

#### 3.3.3 DataRingV2 frame header

```c
struct FrameHeaderV2 {
    uint32_t version;               // = 2
    uint32_t header_bytes;
    uint64_t generation;
    uint64_t sequence;              // 単調増加
    uint64_t producer_ts_ns;
    uint32_t stream_id;             // v2では0固定
    uint32_t flags;                 // bit0=CONFIG_BOUNDARY, bit1=DROPPED_PRIOR
    uint32_t channel_count;
    uint32_t block_length_samples;
    uint32_t payload_bytes;
    uint32_t header_crc32c;         // 必須
    uint32_t payload_crc32c;        // feature_flagsで有効時のみ
    uint32_t reserved;
    // payload: [ch0][ch1]...[chN-1] (channel-major)
};
```

v2 既定値:
- `block_length_samples = 65536`
- `slot_count = 64`
- `sequence gap 許容 = 0` (gap は drop として記録)

#### 3.3.4 フロー制御/通知/同期

- credit-based window を必須とする:
  - `inflight = write_sequence - read_sequence`
  - `inflight < credits_granted` の場合のみ producer は publish 可能
- credit 枯渇時の方針は **drop-new**（loss-tolerant realtime）
- 同期:
  - SG は payload 完了後 header 確定、最後に write index を release publish
  - Main は acquire load 後に header CRC 検証、問題なければ payload を読む
- 通知:
  - 必須: poll mode
  - 任意: `doorbell_event`（eventfd/OS event）を `feature_flags` で有効化

#### 3.3.5 ハンドシェイク/状態遷移

1. SG 起動:
- `generation` 採番
- DataRingV2 作成
- ControlBlockV2 更新 (`ready=1`)

2. Main auto-spawn/attach:
- `grebe-ipc-ctrl` を読んで `ring_desc`/`active_config`/`generation` を取得
- `grebe-ipc-cons` を初期化 (`credits_granted` 設定)
- DataRingV2 attach

3. 異常/復帰:
- child モード: プロセス終了検知で degraded
- attach モード: heartbeat timeout で degraded
- generation 更新検出で reattach

4. cleanup:
- SG は旧 `grebe-ipc-data-<generation>` を unlink/close
- Main は旧セグメントを detach し best-effort cleanup

#### 3.3.6 タイムアウト/しきい値 (v2 既定値)

| 項目 | 既定値 |
|---|---|
| producer heartbeat 更新周期 | 100 ms |
| consumer heartbeat 更新周期 | 100 ms |
| heartbeat timeout 判定 | 500 ms |
| attach 初期待機タイムアウト | 5 s |
| block_length_samples | 65536 |
| slot_count | 64 |
| 初期 credits_granted | 32 |

### 3.4 標準I/Fとの対応

| 観点 | 本IPC(v2) | 参照パターン |
|---|---|---|
| データリング | 固定長 descriptor-like ring | AF_XDP/DPDK |
| 制御/データ分離 | ControlBlock + DataRing 分離 | Aeron |
| フロー制御 | credit window | PCIe/NVMe queue depth |
| 通知 | poll + optional doorbell | NVMe/xHCI |
| 信頼性 | loss-tolerant realtime + drop accounting | UDP realtime profile |

### 3.5 トリガベース捕捉と同期整合性（SG/Main 両側）

表示フレームの時間整合性を確保するため、`grebe-sg` と `grebe` の双方で
trigger-aware な捕捉を行う。

- SG 側（source authority）:
  - internal trigger / external trigger / periodic timer を提供する
  - trigger 判定は SG のサンプル生成クロック上で実施する
  - capture window（pre/post trigger）単位でデータを送出し、境界情報を付与する
- Main 側（render authority）:
  - 受信した capture 境界と sequence 連続性を検証する
  - frame validity を判定し、invalid frame を明示表示する
  - timer fallback 時は window coverage を品質指標として継続監視する

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
  - JSON レポート出力、pass/fail 判定（≥30 FPS）
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
| Trigger Mode | enum | internal / external / timer |
| Trigger Lock | bool | trigger成立状態（armed/locked） |
| Frame Validity | enum / % | valid/invalid と valid frame 比率 |
| Window Coverage | % | capture window 充足率 |
| SG Drops | count | SG 側で発生した累積ドロップ |

### FR-07: マルチチャンネル表示

- **FR-07.1:** `--channels=N` (N=1,2,4,8) で起動時にチャンネル数を指定可能にする（デフォルト: 1）
- **FR-07.2:** 各チャンネルを画面内の独立した垂直レーンに描画する
- **FR-07.3:** 各チャンネルに固有の色を割り当てる（Ch0=緑, Ch1=黄, Ch2=シアン, Ch3=マゼンタ 等）
- **FR-07.4:** 各チャンネルで独立に間引き処理を行う
- **FR-07.5:** N=1 のとき従来と同一の動作を保証する（後方互換）

### FR-08: プロセス分離と起動制御

- **FR-08.1:** 可視化メインを `grebe`、信号生成を `grebe-sg` の別実行バイナリとして提供する
- **FR-08.2:** `grebe` はデフォルトで `grebe-sg` を子プロセスとして自動起動する
- **FR-08.3:** 既存 `grebe-sg` へ接続する attach モードを提供する
- **FR-08.4:** `grebe-sg` の停止・再起動を `grebe` が検知し、復帰可能にする
  - child モード: プロセスハンドル監視（wait/poll）
  - attach モード: heartbeat timeout 監視
  - 共有メモリの stale データ判定に generation counter を用いる
- **FR-08.5:** 可視化UIに信号生成設定UIを持ち込まず、責務分離を維持する
- **FR-08.6:** Phase 8 で `grebe` + `grebe-sg` の最小E2E動作（接続・表示）を必須とする
  - production Shared Memory 前でも、stub/暫定 transport で runnable を満たす

### FR-09: SG UI とチャンネル設定モデル

- **FR-09.1:** SG UI は専用ウィンドウで動作する
- **FR-09.1a:** SG UI 描画バックエンドは `GLFW + OpenGL + ImGui` を採用する（Vulkan は不要）
- **FR-09.2:** チャンネル数は 1-8 をサポートする
- **FR-09.3:** サンプルレートはグローバル設定（全チャンネル共通）とする
- **FR-09.4:** 各チャンネルごとに modulation/waveform を独立設定可能にする
- **FR-09.5:** data length (block length) は初期実装で全チャンネル共通値とする（固定長フレーミング）
- **FR-09.6:** 各チャンネル独立の可変 data length は拡張要件として後続フェーズで扱う
- **FR-09.7:** SG 側に trigger mode 設定を追加する（internal / external / periodic timer）
- **FR-09.8:** internal trigger は level/edge（rising/falling/both）を設定可能にする
- **FR-09.9:** SG 側は trigger をサンプル生成クロックで判定し、capture window 境界を IPC で通知する
- **FR-09.10:** external trigger 未入力時は periodic timer へフォールバック可能にする（設定で有効/無効）

### FR-10: トランスポート抽象化と計測

- **FR-10.1:** `grebe-sg` → `grebe` のデータ搬送は抽象 I/F 経由で実装する
- **FR-10.2:** 初期実装は Shared memory + `memcpy` とする
- **FR-10.3:** 外部インターフェース帯域/遅延評価のため、将来的な代替バックエンドを差し替え可能にする
- **FR-10.4:** attach/discovery 用に `ControlBlockV2` (`grebe-ipc-ctrl`) を定義する
  - `SignalConfigV2`
  - `DataRingDescV2`
  - producer heartbeat
  - generation
- **FR-10.5:** consumer 状態共有用に `ConsumerStatusBlockV2` (`grebe-ipc-cons`) を定義する
  - `read_sequence`
  - `credits_granted`
  - consumer heartbeat
- **FR-10.6:** フロー制御は credit-based window を必須とする
- **FR-10.7:** 信頼性プロファイルは loss-tolerant realtime とし、credit 枯渇時は drop-new とする
- **FR-10.8:** `FrameHeaderV2` の `header_crc32c` 検証を必須とする
- **FR-10.9:** 初期実装時点で最低限の transport 指標（throughput, drop rate, enqueue/dequeue, inflight depth, credits utilization）を計測する
- **FR-10.10:** E2E timestamp delta と `--profile` JSON/CSV 統合は後続フェーズで追加する
- **FR-10.11:** config 更新は generation bump 経由のみ許可し、in-place config 書き換えは行わない
- **FR-10.12:** trigger/capture 境界メタデータ（trigger mode, trigger ts, capture boundary）を伝搬可能にする

### FR-11: フレーム有効性判定と波形整合性

- **FR-11.1:** Main 側で frame validity 判定を必須化する（valid / invalid）
- **FR-11.2:** validity 判定条件として以下を最低限含める
  - sequence continuity（欠落検知）
  - CRC 検証（header/payload）
  - capture window 充足率（不足時 invalid）
- **FR-11.3:** invalid frame は HUD/ログ/プロファイルで明示し、silent success を禁止する
- **FR-11.4:** 頂点数/FPSとは独立に、波形整合性メトリクスを評価する
  - envelope mismatch rate（Embedded 参照比較）
  - peak miss rate
  - extremum amplitude error（p50/p95/p99）
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

### NFR-02b: トリガ整合性

- trigger lock 後の invalid frame 率を継続監視する
- periodic timer モードでは window coverage の下限しきい値を定義する（例: 95%）
- PoC 判定は FPS/頂点数に加えて FR-11.4 の整合性メトリクスを含める

### NFR-03: メモリ使用量

- Ring buffer サイズ: デフォルト 16M samples (32 MB)、1 GSPS 時 64M+ 推奨
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

- `grebe-sg` の異常終了時、`grebe` はクラッシュせず状態を degraded 表示して待機する
- `grebe-sg` の再起動後、`grebe` は再接続して描画を再開できる
- プロセス境界導入後も 1時間連続実行でリーク/ハングがないこと
- 固定長フレーミングでの共有メモリリングは sequence 欠落を検知できること
- credit window 制御下で producer/consumer がデッドロックしないこと
- header CRC 異常を確実に検出し、破損フレームを破棄できること

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

| オプション | 説明 |
|---|---|
| `--log` | CSV テレメトリを `./tmp/` に出力 |
| `--profile` | 自動プロファイリング実行、JSON レポート出力 |
| `--bench` | 独立マイクロベンチマーク実行、JSON 出力 |
| `--ring-size=<N>[K\|M\|G]` | Ring buffer サイズ指定（デフォルト 16M） |
| `--channels=N` | チャンネル数指定（デフォルト 1, 最大 8） |
| `--attach-sg` | `grebe-sg` を自動起動せず既存プロセスへ接続 |
| `--sg-path=<path>` | 自動起動時に使用する `grebe-sg` 実行ファイルパス |
| `--transport=<name>` | トランスポート実装選択（現時点 `shared_mem` のみ。将来拡張予約） |
| `--trigger-mode=<internal\|external\|timer>` | 捕捉モード指定（デフォルト: timer） |
| `--pre-trigger=<samples>` | pre-trigger サンプル数 |
| `--post-trigger=<samples>` | post-trigger サンプル数 |
| `--timer-period-ms=<ms>` | periodic timer 捕捉周期 |

### CLI オプション (`grebe-sg`)

| オプション | 説明 |
|---|---|
| `--channels=N` | チャンネル数指定（デフォルト 1, 最大 8） |
| `--sample-rate=<Hz>` | グローバルサンプルレート初期値 |
| `--transport=<name>` | トランスポート実装選択（現時点 `shared_mem` のみ。将来拡張予約） |
| `--segment-name=<name>` | IPC 共有メモリ名 |
| `--trigger-mode=<internal\|external\|timer>` | SG 側 trigger モード |
| `--trigger-level=<int16>` | internal trigger 閾値 |
| `--trigger-edge=<rising\|falling\|both>` | internal trigger エッジ条件 |
| `--timer-period-ms=<ms>` | timer trigger 周期 |
| `--ext-trigger-source=<name>` | external trigger 入力源（将来拡張予約） |

---

## 8. 成果物

| 成果物 | 形式 | 説明 |
|---|---|---|
| ソースコード | C++ / CMake | ビルド可能なPoC一式 |
| プロファイルレポート | JSON | 4シナリオの自動計測結果 |
| マイクロベンチマーク結果 | JSON | BM-A/B/C/E の計測値 |
| テレメトリログ | CSV | フレーム単位の詳細計測値 |
| 技術的判断メモ | Markdown | TI-01〜06 への回答と推奨事項 |

---

## 9. リスクと制約

| リスク | 影響 | 結果 |
|---|---|---|
| 1GSPS の CPU 間引きが追いつかない | L2 未達 | **緩和済み**: MinMax SIMD ~20 GSPS で 20x マージン |
| PCIe 帯域が律速 | L2 未達 | **設計で回避**: 間引き後 7.68 KB/frame のみ転送 |
| Vulkan 初期化の複雑さ | 全体遅延 | **緩和済み**: vk-bootstrap 活用 |
| GPU ベンダー間の挙動差 | 再現性低下 | **検証済み**: RTX 5080 で複数環境動作確認 |
| LTTB が高レートで追いつかない | 描画落ち | **対策済み**: ≥100 MSPS で自動無効化 |
| 親子プロセス管理の複雑化 | 起動失敗/孤児プロセス | **要対策**: spawn/attach 両経路の状態遷移を明文化 |
| IPC 同期不整合（seq 欠落、再接続） | データ欠損/停止 | **要対策**: sequence + timestamp + timeout/reconnect |
| トリガ未ロック/誤トリガ | 無効波形表示 | **要対策**: SG側trigger判定 + Main側validity判定 + timer fallback |

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
