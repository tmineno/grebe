# Grebe — 開発マイルストーン (v0.3.0–)

> v0.2.0 (Phase 0–9.2) の成果は [TR-001](TR-001.md) / [TR-002](TR-002.md) に記録済み。
> 本書は RDD v3.1 Target Architecture の実装ロードマップを定義する。

---

## v0.2.0 完了状態サマリ

| 領域 | 状態 |
|------|------|
| データパイプライン (libgrebe) | IngestionThread → RingBuffer → DecimationThread の固定パイプライン |
| データソース抽象 | `IDataSource` (SyntheticSource, TransportSource) |
| 描画抽象 | `IRenderBackend` (VulkanRenderer) |
| トランスポート | Pipe / UDP (scatter-gather, sendmmsg/recvmmsg) |
| Queue | SPSC RingBuffer (in-process のみ、Owned のみ) |
| Stage 抽象 | **なし** — 処理単位はドメイン固有の固定クラス |
| Runtime | **なし** — スレッドはハードワイヤード |
| SharedMemory | **なし** |
| Fan-out | **なし** |
| Frame 所有権 | Owned のみ（Borrowed 未実装） |

---

## ターゲットファイル構造

v0.3.0 完了時点で想定するディレクトリ構成。
既存ファイルは原則移動せず、新規ディレクトリを段階的に追加する。

```
include/grebe/
  grebe.h                         (既存 — umbrella header を更新)
  config.h                        (既存)
  telemetry.h                     (既存 — Stage 単位 telemetry に拡張)
  data_source.h                   (既存 — legacy, v4.0 廃止予定)
  render_backend.h                (既存 — legacy, v4.0 廃止予定)
  decimation_engine.h             (既存 — legacy, v4.0 廃止予定)
  stage.h                         (Phase 10 — IStage, StageResult)
  frame.h                         (Phase 10 — Frame, OwnershipModel)
  batch.h                         (Phase 10 — BatchView, BatchWriter, ExecContext)
  queue.h                         (Phase 11 — IQueue, BackpressurePolicy)
  runtime.h                       (Phase 13 — IRuntime, StageGraph)

src/
  # 既存: データパイプラインコア (変更なし、v4.0 まで維持)
  ring_buffer.h, ring_buffer_view.h
  decimator.h/cpp
  decimation_thread.h/cpp
  decimation_engine.cpp
  ingestion_thread.h/cpp
  synthetic_source.h/cpp
  drop_counter.h
  waveform_type.h, waveform_utils.h

  # 新規: コア Runtime (Phase 11–13)
  core/
    in_process_queue.h/cpp        (Phase 11)
    stage_graph.h/cpp             (Phase 13)
    linear_runtime.h/cpp          (Phase 13)

  # 新規: 組み込み Stage 実装 (Phase 12)
  stages/
    data_source_adapter.h/cpp     (Phase 12 — IDataSource → IStage)
    decimation_stage.h/cpp        (Phase 12 — Decimator ラッパー)
    visualization_stage.h/cpp     (Phase 12 — IRenderBackend ラッパー)

  # 新規: 共有メモリシステム (Phase 14)
  shm/
    shm_region.h/cpp              (Phase 14 — POSIX shm / Win32 共有メモリ)
    shm_queue.h/cpp               (Phase 14 — IQueue の SharedMemory backing)
    shm_reader.h/cpp              (Phase 14 — SourceStage)
    shm_writer.h/cpp              (Phase 14 — SinkStage)

apps/
  common/
    ipc/                          (既存 — transport 実装)
    stages/                       (Phase 12 — transport Stage ラッパー)
      transport_rx_stage.h/cpp    (Phase 12 — ITransportConsumer → IStage)
      transport_tx_stage.h/cpp    (Phase 12 — ITransportProducer → IStage)

  viewer/                         (既存 — Phase 13 で Runtime ベースに移行)
  sg/                             (既存 — Phase 14 で ShmWriter 対応)
  bench/                          (既存 — Phase 16 で NFR 検証拡張)
```

**設計判断:**
- `src/core/`: ドメイン非依存の Runtime インフラ
- `src/stages/`: libgrebe 内蔵 Stage (外部依存なし — Vulkan 等は含まない)
- `src/shm/`: OS 固有コードをここに隔離
- `apps/common/stages/`: transport は `ipc/` に依存するため libgrebe には入れない
- 既存 `src/` 直下のファイルは移動しない (v4.0 で IStage ネイティブ化時に整理)

---

## Phase 10: IStage 契約と Frame 所有権 (FR-01, FR-12)

RDD §5.1–5.2 の Stage 契約と Frame 所有権モデルを libgrebe に導入する。
既存のパイプラインは変更せず、新しい抽象を並行して定義する。

### ファイル操作

**新規作成:**

| ファイル | 内容 |
|---|---|
| `include/grebe/stage.h` | `IStage`, `StageResult` (Ok/EOS/Retry/Error) |
| `include/grebe/frame.h` | `Frame`, `OwnershipModel` (Owned/Borrowed), Borrowed→Owned 変換 |
| `include/grebe/batch.h` | `BatchView`, `BatchWriter`, `ExecContext` |

**変更:**

| ファイル | 内容 |
|---|---|
| `include/grebe/grebe.h` | 新ヘッダの `#include` を追加 |
| `CMakeLists.txt` | libgrebe に `frame.cpp` を追加（必要な場合） |

### タスク

- [x] `IStage` インタフェイス定義
  - `StageResult process(const BatchView& in, BatchWriter& out, ExecContext& ctx)`
- [x] `Frame` 型定義
  - channel-major `int16_t` データ、sequence、producer_ts、sample_rate_hz
  - 所有権フラグ (Owned / Borrowed)
  - Borrowed → Owned 変換 (コピー)
- [x] `BatchView` / `BatchWriter` / `ExecContext` 定義
- [x] 既存 `FrameBuffer` との互換レイヤー
  - `Frame` ↔ `FrameBuffer` 変換ユーティリティ

**受入条件:**
- `IStage` を実装した最小のテスト Stage (pass-through) がビルド・実行可能であること
- Owned Frame の生成・消費・破棄が動作すること
- 既存パイプライン (grebe-viewer, grebe-sg) が変更なしでビルド・動作すること

---

## Phase 11: Queue 契約と Backpressure (FR-07)

RDD §5.3 の Queue 契約を libgrebe に導入する。
既存の SPSC RingBuffer を in-process backing として再利用する。

### ファイル操作

**新規作成:**

| ファイル | 内容 |
|---|---|
| `include/grebe/queue.h` | `IQueue<Frame>`, `BackpressurePolicy` |
| `src/core/in_process_queue.h` | `InProcessQueue` 宣言 |
| `src/core/in_process_queue.cpp` | `InProcessQueue` 実装 |

**変更:**

| ファイル | 内容 |
|---|---|
| `CMakeLists.txt` | libgrebe に `src/core/in_process_queue.cpp` を追加 |

### タスク

- [x] `src/core/` ディレクトリ作成
- [x] `IQueue<Frame>` インタフェイス定義
  - `enqueue(Frame&&)` / `dequeue() → optional<Frame>`
  - capacity、fill ratio の公開
- [x] `BackpressurePolicy` 定義: `drop_latest`, `drop_oldest`, `block`
- [x] `InProcessQueue` 実装
  - std::deque + mutex + condition_variable (Frame は move-only のため RingBuffer 不使用)
  - policy 適用結果を telemetry に反映

**受入条件:**
- `InProcessQueue` が `IQueue` 契約を満たし、3 種の backpressure policy で動作すること
- 既存パイプラインが変更なしでビルド・動作すること

---

## Phase 12: 既存コンポーネントの Stage ラッピング (FR-04, FR-05, FR-06)

既存の処理コンポーネントを `IStage` 実装としてラップする。
`IDataSource` アダプタを設け、v3.x 期間中の後方互換を維持する。

### ファイル操作

**新規作成:**

| ファイル | 内容 |
|---|---|
| `src/stages/data_source_adapter.h` | `DataSourceAdapter` 宣言 (IDataSource → IStage) |
| `src/stages/data_source_adapter.cpp` | 同上 実装 |
| `src/stages/decimation_stage.h` | `DecimationStage` 宣言 |
| `src/stages/decimation_stage.cpp` | 同上 実装 (Decimator をラップ) |
| `src/stages/visualization_stage.h` | `VisualizationStage` 宣言 |
| `src/stages/visualization_stage.cpp` | 同上 実装 (IRenderBackend をラップ) |
| `apps/common/stages/transport_rx_stage.h` | `TransportRxStage` 宣言 |
| `apps/common/stages/transport_rx_stage.cpp` | 同上 実装 (ITransportConsumer をラップ) |
| `apps/common/stages/transport_tx_stage.h` | `TransportTxStage` 宣言 |
| `apps/common/stages/transport_tx_stage.cpp` | 同上 実装 (ITransportProducer をラップ) |

**変更:**

| ファイル | 内容 |
|---|---|
| `CMakeLists.txt` | libgrebe に `src/stages/*.cpp` を追加 |
| `CMakeLists.txt` | grebe-viewer / grebe-sg に `apps/common/stages/*.cpp` を追加 |
| `CMakeLists.txt` | grebe-viewer / grebe-sg の include path に `apps/common` を追加（既存） |

### タスク

- [x] `src/stages/` ディレクトリ作成
- [x] `apps/common/stages/` ディレクトリ作成
- [x] `DataSourceAdapter`: `IDataSource` → `IStage` アダプタ
  - SyntheticSource、TransportSource をそのまま Stage として利用可能に
- [x] `DecimationStage`: Decimator を ProcessingStage としてラップ
- [x] `VisualizationStage`: `IRenderBackend` を VisualizationStage としてラップ
  - Vulkan 依存なし（IRenderBackend ポインタを受け取るのみ）
- [x] `TransportRxStage`: Pipe/UDP 受信を SourceStage としてラップ
  - `apps/common/ipc/` の `ITransportConsumer` に依存
- [x] `TransportTxStage`: Pipe/UDP 送信を SinkStage としてラップ
  - `apps/common/ipc/` の `ITransportProducer` に依存

**受入条件:**
- 各 Stage が `IStage::process()` 契約を満たすこと
- DataSourceAdapter 経由で SyntheticSource / TransportSource が動作すること
- 既存の E2E パイプライン (Embedded / Pipe / UDP) が Stage ラッピング後も動作すること

---

## Phase 13: Runtime 基盤 (FR-02, FR-03)

RDD §4.2 の Runtime 責務を実装する。
まず線形パイプラインの実行を Runtime で管理し、既存のハードワイヤードスレッドを置換する。

### ファイル操作

**新規作成:**

| ファイル | 内容 |
|---|---|
| `include/grebe/runtime.h` | `LinearRuntime`, `StageTelemetry` |
| `src/core/linear_runtime.h` | `LinearRuntime::Impl` 内部宣言 |
| `src/core/linear_runtime.cpp` | `LinearRuntime` 実装 (worker, telemetry, poll) |

**変更:**

| ファイル | 内容 |
|---|---|
| `CMakeLists.txt` | libgrebe に `src/core/linear_runtime.cpp` を追加 |
| `src/stages/decimation_stage.h/cpp` | `set_sample_rate()`, `effective_mode()`, LTTB guard, `sample_rate_hz` 比例調整 |
| `src/stages/visualization_stage.h/cpp` | 表示ウィンドウイング + 再デシメーション (per-channel deque 蓄積) |
| `apps/viewer/app_loop.h/cpp` | `LinearRuntime` + `VisualizationStage` ベースのパイプラインに変更 |
| `apps/viewer/main.cpp` | Runtime 初期化・停止シーケンスに変更 |
| `apps/viewer/hud.cpp` | Ring% 削除 |

### タスク

- [x] `LinearRuntime` 実装 (`include/grebe/runtime.h`, `src/core/linear_runtime.h/.cpp`)
  - Stage ごとの worker スレッド管理
  - 停止シーケンス (bounded time, queue shutdown + join)
  - Stage 単位の telemetry 収集 (frames_processed, avg_process_time_ms, queue_dropped)
  - `poll_output()` / `poll_latest()` で出力フレームを取得
  - Note: `IRuntime` / `StageGraph` は v3.2 以降で追加予定、現段階は `LinearRuntime` 直接使用
- [x] `DecimationStage` に `set_sample_rate()` / `effective_mode()` 追加
  - LTTB 高レートガード (≥100 MSPS → MinMax 自動切替)
- [x] grebe-viewer を `LinearRuntime` ベースに移行
  - `DataSourceAdapter >> InProcessQueue >> DecimationStage >> output Queue >> main thread polls`
  - 既存の `IngestionThread` + `RingBuffer` + `DecimationEngine` を置換
  - Embedded / Pipe / UDP の全モードで動作検証済み
- [x] `VisualizationStage` 実装 (表示ウィンドウイング + 再デシメーション)
  - パイプラインデシメーション (データ量削減) と表示デシメーション (time span 対応) の分離
  - per-channel deque によるサンプル蓄積 → `visible_time_span` ウィンドウ → MinMax 再デシメーション
  - `sample_rate_hz` 比例調整による時間軸整合
  - `poll_output()` ループによる全フレームドレイン (位相連続性の維持)
- [x] Profiler Envelope 検証の Stage モデル対応
  - pipeline 出力 (単一 MinMax) を profiler に渡す (display frame は二重デシメーションのため不適)
  - `per_channel_raw_counts` をレート比から復元 (`data_rate / decimated_rate × spc`)
  - 全 4 シナリオ (1M/10M/100M/1G SPS) で Env% = 100% 確認
- [x] HUD から Ring% を削除 (Stage モデルでは Ring Buffer 不在)

**受入条件:**
- grebe-viewer が `LinearRuntime` 経由で Embedded モードの波形描画が動作すること
- Pipe / UDP モードも Stage 構成で動作すること
- Stage 単位の telemetry (throughput, latency) が HUD に表示されること
- 既存の性能 (NFR-01: 1ch × 1 GSPS ≥ 60 FPS) を維持すること
- `--profile` で全シナリオ Env% = 100%, 全 PASS

---

## Phase 14: SharedMemory Queue (FR-10)

RDD §5.3 SharedMemory backing の Queue を実装する。
同一マシン上のプロセス間でゼロコピー通信を可能にする。

### ファイル操作

**新規作成:**

| ファイル | 内容 |
|---|---|
| `src/shm/shm_region.h` | `ShmRegion` 宣言 (共有メモリ領域管理) |
| `src/shm/shm_region.cpp` | POSIX `shm_open` / Win32 `CreateFileMapping` 実装 |
| `src/shm/shm_queue.h` | `ShmQueue` 宣言 (IQueue の SharedMemory backing) |
| `src/shm/shm_queue.cpp` | atomic + fence による整合性、参照カウント |
| `src/shm/shm_reader.h` | `ShmReader` SourceStage 宣言 |
| `src/shm/shm_reader.cpp` | Borrowed Frame 生成、borrow/release |
| `src/shm/shm_writer.h` | `ShmWriter` SinkStage 宣言 |
| `src/shm/shm_writer.cpp` | payload pool への書き込み、descriptor 配布 |

**変更:**

| ファイル | 内容 |
|---|---|
| `CMakeLists.txt` | libgrebe に `src/shm/*.cpp` を追加、`-lrt` リンク (Linux) |
| `apps/viewer/main.cpp` | `--shm` CLI オプション追加、ShmReader ベースのパイプライン構築 |
| `apps/sg/sg_main.cpp` | `--shm` CLI オプション追加、ShmWriter ベースの出力 |

### タスク

- [ ] `src/shm/` ディレクトリ作成
- [ ] `ShmRegion`: 共有メモリ領域の確保・マッピング
  - 初期化時にサイズ固定 (NFR-08)
- [ ] `ShmQueue`: SharedMemory backing の `IQueue` 実装
  - atomic + memory fence による整合性
  - Borrowed Frame の borrow/release セマンティクス
  - 参照カウントベースの payload 再利用
- [ ] `ShmWriter` (SinkStage): SourceStage 出力を共有メモリに書き込み
- [ ] `ShmReader` (SourceStage): 共有メモリからデータを読み取り
- [ ] 障害検知
  - コンシューマ crash: heartbeat/タイムアウトで切り離し (NFR-06: ≤ 1 秒)
  - プロデューサ crash: EOS 検知、graceful 停止
- [ ] grebe-sg / grebe-viewer に `--shm` モードを追加

**受入条件:**
- 2 プロセス構成 (grebe-sg → ShmWriter / ShmReader → grebe-viewer) で波形描画が動作すること
- Borrowed Frame がコピーなしで ProcessingStage に渡ること (NFR-04)
- 定常状態でヒープ確保が 0 であること (NFR-08)
- コンシューマ crash 後 1 秒以内にプロデューサが検知し継続動作すること (NFR-06)

---

## Phase 15: Fan-Out (FR-11)

RDD §4.3 の fan-out パターンを Runtime に実装する。
一つの Stage 出力を複数の下流 Stage に同時配信する。

### ファイル操作

**変更:**

| ファイル | 内容 |
|---|---|
| `include/grebe/runtime.h` | `StageGraph` に fan-out API 追加 |
| `src/core/stage_graph.h/cpp` | fan-out ルーティング実装 |
| `src/core/linear_runtime.h/cpp` | fan-out worker 管理 |
| `src/shm/shm_queue.h/cpp` | 参照 descriptor の複数 Queue 配布 |

### タスク

- [ ] `StageGraph` に fan-out 構文を追加
  - `source >> fanout(processing_a, processing_b, sink_c)`
- [ ] コンシューマごとの独立 Queue 生成
  - 各 Queue に独立した backpressure policy
- [ ] 遅い消費者の分離 (NFR-05)
  - 遅延コンシューマが他系統をブロックしない
- [ ] In-process fan-out: Frame コピー or clone
- [ ] SharedMemory fan-out: 共有 payload pool + 参照 descriptor 配布

**受入条件:**
- 1 Source → 2 Consumer (Decimation + Recorder) の fan-out が動作すること
- 遅い消費者の追加・削除がプロデューサ停止なしに行えること (NFR-05)
- SharedMemory fan-out でプロデューサの payload 書き込みが 1 回であること

---

## Phase 16: grebe-bench NFR 検証基盤 (NFR-00)

RDD §7 の NFR 階層に対応した自動検証基盤を grebe-bench に構築する。

### ファイル操作

**新規作成:**

| ファイル | 内容 |
|---|---|
| `apps/bench/bench_shm.h/cpp` | SharedMemory システム NFR ベンチマーク |
| `apps/bench/bench_ingress.h/cpp` | SourceStage ingress NFR ベンチマーク |
| `apps/bench/bench_vis.h/cpp` | VisualizationStage NFR ベンチマーク |

**変更:**

| ファイル | 内容 |
|---|---|
| `apps/bench/main.cpp` | NFR 検証サブコマンド追加 |
| `CMakeLists.txt` | 新規ベンチマークソースを追加 |

### タスク

- [ ] SharedMemory システム NFR 検証 (NFR-02〜NFR-08)
  - E2E レイテンシ、ゼロドロップ、ゼロコピー効率、fan-out 独立性
- [ ] SourceStage ingress NFR 検証 (NFR-09〜NFR-11)
  - Embedded / Pipe / UDP の ingress 性能比較
  - 下流条件を固定した分離評価
- [ ] VisualizationStage NFR 検証 (NFR-01, NFR-12)
  - 描画性能、処理レイテンシ
- [ ] JSON レポート出力 + 合否判定

**受入条件:**
- `grebe-bench` 単体実行で NFR 階層ごとの合否判定が自動出力されること
- SharedMemory: 1ch × 1 GSPS, 0 drops, p99 ≤ 50 ms
- VisualizationStage: 1ch × 1 GSPS ≥ 60 FPS

---

## Phase 17: クロスプラットフォーム検証

Linux (GCC) と Windows (MSVC) で Stage/Runtime 基盤の動作と性能を検証する。

### ファイル操作

**変更:**

| ファイル | 内容 |
|---|---|
| `src/shm/shm_region.cpp` | Win32 `CreateFileMapping` パスの実装・検証 |
| `scripts/build-windows.sh` | 新規ソースファイルへの対応確認 |

### タスク

- [ ] Windows MSVC ビルド検証 (libgrebe + 全アプリ)
- [ ] SharedMemory Queue の Windows 実装検証
- [ ] Linux/Windows 性能差 ≤ 20% の確認
- [ ] grebe-bench NFR テストの両環境通過

**受入条件:**
- Linux と Windows で同一ソースからビルド・動作すること
- 両環境で grebe-bench NFR が PASS すること

---

## 将来フェーズ (Planned)

| フェーズ | 内容 | RDD バージョン |
|----------|------|----------------|
| NUMA-aware scheduling | Stage のスレッド配置を NUMA ノードに最適化 | v3.3 |
| Stage fusion | 隣接 Stage の融合によるオーバーヘッド削減 | v3.3 |
| Transport backend 抽象統一 | sync/mmsg/iocp/shm を統一インタフェイスに | v3.4 |
| `IDataSource` adapter 廃止 | built-in Source の IStage ネイティブ移行完了、既存 `src/` 直下ファイルの整理 | v4.0 |
| `find_package(grebe)` 対応 | CMake パッケージ配布 | v4.0 |
| API リファレンス (doc/API.md) | 公開 API ドキュメント | v4.0 |

---

## 達成状況サマリ

| Phase | 内容 | 主要 FR/NFR | ステータス |
|-------|------|-------------|-----------|
| 10. IStage 契約と Frame 所有権 | Stage 抽象の基盤 | FR-01, FR-12 | **完了** |
| 11. Queue 契約と Backpressure | Stage 間接続の基盤 | FR-07 | **完了** |
| 12. Stage ラッピング | 既存コンポーネントの移行 | FR-04, FR-05, FR-06 | **完了** |
| 13. Runtime 基盤 | Stage グラフ実行エンジン | FR-02, FR-03 | **完了** |
| 14. SharedMemory Queue | プロセス間ゼロコピー通信 | FR-10 | 未着手 |
| 15. Fan-Out | マルチコンシューマ配信 | FR-11 | 未着手 |
| 16. grebe-bench NFR 検証 | 自動性能検証基盤 | NFR-00〜NFR-12 | 未着手 |
| 17. クロスプラットフォーム検証 | Windows/Linux 検証 | — | 未着手 |
