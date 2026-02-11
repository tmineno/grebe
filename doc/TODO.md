# Grebe — 開発マイルストーン

> PoC (v0.1.0) の成果は [TR-001](TR-001.md) に記録済み。PoC コードベースは `v0.1.0` タグで参照可能。

---

## Phase 1: プロジェクト構造再編 ✅

PoC のコードベースを libgrebe + grebe-viewer の構成に分離する。

- [x] ディレクトリ構成: `include/grebe/` (公開ヘッダ), `src/` (内部実装), `apps/viewer/`, `apps/bench/`
- [x] CMake 再構成: `libgrebe` (STATIC), `grebe-viewer` (exe), `grebe-bench` (exe)
- [x] コア公開ヘッダ定義: `grebe/grebe.h`, `grebe/data_source.h`, `grebe/render_backend.h`, `grebe/config.h`, `grebe/telemetry.h`
- [x] 既存 PoC コードがリファクタ後もビルド・実行可能であること

---

## Phase 2: IDataSource 抽象化 (FR-01) ✅

DataGenerator を IDataSource 実装に分離し、データソースの差し替え可能な構造にする。

- [x] IDataSource インタフェイス定義 (`data_source.h`)
- [x] SyntheticSource 実装 (PoC DataGenerator からの移行)
- [x] IpcSource 実装 (grebe-sg IPC パイプ経由)
- [x] パイプラインが IDataSource 経由でデータを受け取る構成への変更

---

## Phase 3: IRenderBackend 抽象化 (FR-04) ✅

Renderer を IRenderBackend 実装に分離し、描画バックエンドの差し替え可能な構造にする。

- [x] IRenderBackend インタフェイス定義 (`render_backend.h`): DrawCommand, DrawRegion
- [x] VulkanRenderer 実装 (PoC Renderer からの移行)
- [x] パイプラインが IRenderBackend 経由で描画する構成への変更

---

## Phase 4: DecimationEngine API 公開 (FR-02, FR-03) ✅

デシメーション処理を公開 API として整理し、アルゴリズム拡張ポイントを設ける。

- [x] DecimationEngine 公開 API 定義 (DecimationAlgorithm, DecimationConfig, DecimationOutput, DecimationMetrics)
- [x] MinMax (SSE2 SIMD) + LTTB アルゴリズム移行
- [x] ≥ 100 MSPS での自動 MinMax 切替維持
- [x] マルチチャネル (1–8ch) 移行

---

## Phase 5: Config API + Telemetry (FR-05, FR-07) ✅

構造体ベースの設定 API とテレメトリ公開 API を整備する。

- [x] PipelineConfig 構造体 (チャネル数、リングサイズ、デシメーション設定、V-Sync)
- [x] TelemetrySnapshot メトリクス構造体 (FPS, E2E レイテンシ, 充填率, デシメーション時間等)
- [x] grebe-viewer が TelemetrySnapshot 経由で HUD 表示する構成

---

## Phase 6: libgrebe デカップリング ✅

ライブラリからアプリ専用コードを除去し、依存関係を最小化する。

- [x] HUD (ImGui) をライブラリから完全除去 — Renderer→HUD 結合を汎用 overlay callback に置換
- [x] アプリ専用モジュールを `apps/viewer/` に移動: profiler, envelope_verifier, app_command, process_handle, hud
- [x] imgui_lib をライブラリのリンク依存から除去 (grebe-viewer 側でリンク)
- [x] WaveformType enum を独立ヘッダに抽出 (synthetic_source.h が data_generator.h に依存しない)
- [x] `nm -C libgrebe.a | grep "Hud\|ProfileRunner\|ProcessHandle\|ImGui"` → 0

---

## Phase 7: libgrebe をデータパイプラインに純化 ✅

**設計方針:** libgrebe の本質的価値はデータパイプライン (取り込み → デシメーション → 出力) であり、
描画・計測・ロギングは本来アプリの責務。ライブラリを純粋なデータパイプラインに絞る。

### 7a. Vulkan 描画一式の除去

- [x] Vulkan 描画モジュールを `apps/viewer/` に移動:
  - `vulkan_context.h/cpp`, `swapchain.h/cpp`, `renderer.h/cpp`
  - `buffer_manager.h/cpp`, `vulkan_renderer.h/cpp`
  - `compute_decimator.h/cpp`, `vma_impl.cpp`
- [x] libgrebe のリンク依存から除去: Vulkan, GLFW, vk-bootstrap, VMA, glm, stb_headers
- [x] `render_backend.h` (IRenderBackend インタフェイス定義) はライブラリに残留 (ヘッダのみ、リンク依存なし)

### 7b. Benchmark (テレメトリ計測) の除去

- [x] `benchmark.h/cpp` を `apps/viewer/` に移動
  - `TelemetrySnapshot` 構造体は `include/grebe/telemetry.h` に残留 (データ定義のみ)

### 7c. 不要なリンク依存の除去

- [x] `nlohmann_json::nlohmann_json` を libgrebe から除去
- [x] `stb_headers` を libgrebe から除去

### 7d. Phase 7 完了後の libgrebe 構成

**src/ に残るファイル (データパイプラインコア):**
```
ring_buffer.h, ring_buffer_view.h     — SPSC ロックフリーキュー
decimator.h/cpp                       — MinMax (SSE2 SIMD), LTTB アルゴリズム
decimation_thread.h/cpp               — バックグラウンドデシメーションワーカー
decimation_engine.cpp                 — 公開ファサード
data_generator.h/cpp                  — 周期タイリング波形生成
synthetic_source.h/cpp                — IDataSource 実装 (組み込み波形)
ingestion_thread.h/cpp                — DataSource → RingBuffer ドライバ
drop_counter.h                        — ドロップカウンタ
waveform_type.h, waveform_utils.h     — 波形ユーティリティ
```

> **Note:** Phase 7.2 で IPC 関連 (`ipc_source.h/cpp`, `ipc/`) を `apps/` に移動済み。

**リンク依存 (最小):**
```cmake
target_link_libraries(grebe PUBLIC spdlog::spdlog)  # ロギングのみ
```

**検証結果:**
- ✅ libgrebe.a: 147K (PoC 641K → 77% 削減)
- ✅ `nm -C libgrebe.a | grep -c "Vulkan\|vk\|Swapchain\|Benchmark\|IpcSource\|PipeConsumer"` → 0
- ✅ grebe, grebe-viewer, grebe-sg, grebe-bench 全ターゲットがビルド成功

---

## Phase 7.1: リファクタリング性能回帰検証 ✅

Phase 1–7 で導入した抽象化レイヤー（DecimationEngine ファサード、overlay callback、
ファイル移動による再コンパイル）が性能に悪影響を与えていないことを定量検証する。

### 検証結果

| ID | 測定対象 | Delta | 閾値 | 結果 |
|---|---|---|---|---|
| R-1 | MinMax SIMD スループット | +5.5% (21,741 MSPS) | ≤ 5% 劣化 | ✅ PASS |
| R-2 | 描画 FPS (V-Sync OFF) | +0.7% / +1.6% / −1.7% | ≤ 5% 劣化 | ✅ PASS |
| R-3 | overlay callback overhead | ≤ 0.03 ms/frame | ≤ 0.1 ms/frame | ✅ PASS |
| R-4 | DecimationEngine facade | ~10 ns/call (< 0.001 ms) | ≤ 1% 差 | ✅ PASS |
| R-5 | プロファイル FPS/sub-metrics | 全シナリオ PASS、PoC 比ノイズ範囲内 | ≤ 10% 劣化 | ✅ PASS |
| R-6 | 成果物サイズ合計 | −10% (4.6 MB → 減少) | ≤ 5% 増加 | ✅ PASS |

- [x] ベースライン: `bench_20260208_085402.json`, `profile_20260208_085439.json`
- [x] Phase 7 後測定: `bench_20260211_131907.json`, `profile_20260211_131516.json`
- [x] BM-F (overlay callback) を bench suite に追加 — 恒久的な回帰検知に使用可能
- [x] 詳細結果: `doc/TI-phase7.md`

**Overall: PASS — Phase 1–7 リファクタリングによる性能回帰なし**

---

## Phase 7.2: IPC トランスポートのライブラリ除去 ✅

**設計方針:** IPC パイプはデータ取り込みの「一つの具体的なトランスポート手段」であり、
純粋なデータパイプラインライブラリのコア責務ではない。ライブラリは `IDataSource` 抽象のみ提供し、
IPC 実装はアプリ（grebe-viewer / grebe-sg）の責務とする。

### 移動対象

| ファイル | 現在位置 | 消費者 | 移動先 |
|---|---|---|---|
| `ipc_source.h/cpp` | `src/` | `apps/viewer/main.cpp`, `app_loop.cpp` | `apps/viewer/` |
| `transport.h` | `src/ipc/` | ipc_source, pipe_transport, `app_loop.cpp` | `apps/common/ipc/` (共有) |
| `pipe_transport.h/cpp` | `src/ipc/` | `apps/viewer/main.cpp`, `apps/sg/sg_main.cpp` | `apps/common/ipc/` (共有) |
| `contracts.h` | `src/ipc/` | ipc_source, pipe_transport, `apps/sg/`, `apps/viewer/` | `apps/common/ipc/` (共有) |

### 設計

- [x] IPC プロトコル定義 (`contracts.h`, `transport.h`, `pipe_transport.h/cpp`) は grebe-viewer と grebe-sg の両方が使う → `apps/common/ipc/` に共有配置
- [x] `IpcSource` は `IDataSource` 実装だが IPC 固有 → `apps/viewer/` に移動
- [x] libgrebe は `IDataSource` 抽象インタフェイスのみ保持、具体トランスポートを知らない
- [x] CMake: `apps/common/` を include path として grebe-viewer と grebe-sg の双方に追加

### 検証結果

- ✅ `nm -C libgrebe.a | grep -c "IpcSource\|PipeConsumer\|PipeProducer\|FrameHeaderV2"` → 0
- ✅ libgrebe.a: 147K (Phase 7 179K → 18% 削減)
- ✅ grebe, grebe-viewer, grebe-sg, grebe-bench 全ターゲットがビルド成功

### Phase 7.2 完了後の libgrebe 構成

**src/ に残るファイル（純粋データパイプラインのみ）:**
```
ring_buffer.h, ring_buffer_view.h     — SPSC ロックフリーキュー
decimator.h/cpp                       — MinMax (SSE2 SIMD), LTTB アルゴリズム
decimation_thread.h/cpp               — バックグラウンドデシメーションワーカー
decimation_engine.cpp                 — 公開ファサード
data_generator.h/cpp                  — 周期タイリング波形生成
synthetic_source.h/cpp                — IDataSource 実装 (組み込み波形)
ingestion_thread.h/cpp                — DataSource → RingBuffer ドライバ
drop_counter.h                        — ドロップカウンタ
waveform_type.h, waveform_utils.h     — 波形ユーティリティ
```

**リンク依存（最終形）:**
```cmake
target_link_libraries(grebe PUBLIC spdlog::spdlog)
```

**受入条件:**
- `nm -C libgrebe.a | grep -c "IpcSource\|PipeConsumer\|PipeProducer\|FrameHeaderV2"` → 0
- grebe-viewer IPC モード (`build/grebe-viewer`) で grebe-sg 経由のストリーミングが動作すること
- grebe-sg が `apps/common/ipc/` の共有ヘッダでビルドできること

---

## Phase 8: FileSource (FR-01) ✅

grebe-sg にバイナリファイル再生モードを実装。GRB ファイルフォーマット定義。

- [x] DataGenerator を libgrebe (`src/`) から grebe-sg (`apps/sg/`) に移動
- [x] FileReader 実装 (mmap + madvise(SEQUENTIAL), ペーシング, ループ再生)
- [x] sender_thread を DataGenerator から分離 (atomic による疎結合)
- [x] grebe-sg GUI にソース切替 (Synthetic / File) + ファイルロード UI
- [x] grebe-viewer `--file=PATH` → grebe-sg へのパススルー
- [x] GRB ファイルフォーマット仕様 (RDD §3.5)
- [x] テストファイル生成スクリプト (`scripts/generate_grb.py`)

**受入条件:**
- 事前キャプチャ済みバイナリファイルを指定して波形再生できること ✅
- 1ch × 100 MSPS 相当のファイル再生で 60 FPS 動作 ✅

---

## Phase 9: UDP トランスポート (FR-01) ✅

grebe-sg に UDP 送信トランスポートを追加し、grebe-viewer に UdpConsumer 受信を実装。
grebe-sg が既存の波形生成・ファイル再生機能をそのまま UDP 経由で送信できる構成とする。

- [x] IpcSource → TransportSource リネーム (トランスポート非依存の命名)
- [x] UdpProducer / UdpConsumer 実装 (POSIX ソケット / Winsock、外部依存なし)
- [x] grebe-sg に UDP 送信モード追加 (`--transport=pipe|udp`, `--udp-target=HOST:PORT`)
- [x] grebe-sg GUI にトランスポート表示 (読み取り専用)
- [x] grebe-viewer `--udp=PORT` CLI オプション
- [x] UDP ブロックサイズ自動制限 (データグラム 65000 byte 上限)
- [x] Linux + Windows MSVC ビルド検証

**受入条件:**
- grebe-sg (UDP) → UdpConsumer → TransportSource → grebe-viewer でリアルタイム波形描画が動作すること ✅
- ループバックで 1ch × 10 MSPS 以上のストリーミングが安定動作 (0 drops) — E2E 検証待ち

---

## Phase 9.1: UDP モード ボトルネック調査 ✅

UDP トランスポートのスループット上限を定量測定し、ボトルネックを特定する。

- [x] grebe-bench に BM-H (UDP ループバックスループット) 実装
- [x] T-1: フレームレート上限測定 (1ch/2ch/4ch、block_size 変動)
- [x] T-3: フレームサイズ vs スループット特性
- [x] T-1b: 目標レート制限テスト (1M / 10M / 100M / 1G SPS)
- [x] T-5: WSL2 vs Windows native UDP 性能差
- [x] T-5c: Windows MTU 制限解除テスト (65000 byte datagram) — **1 GSPS via UDP 達成**
- [x] T-5d: 4ch マルチチャネル UDP ベンチマーク (Windows native, datagram 1400–65000B)
- [ ] T-2: pipe vs UDP E2E 比較 (viewer FPS)
- [ ] T-4: E2E レイテンシ (UDP モード、各レート)

**測定結果:**
- WSL2 (1400B): ~155K fps / 104 MSPS (1ch max), 0% drops @ 100 MSPS
- Windows native (1400B): ~220K fps / 147 MSPS, 0% drops @ 全レート
- Windows native (65000B): ~104K fps / **3,371 MSPS**, 1 GSPS ターゲット **0% drops で達成**
- Windows native 4ch (65000B): 105K fps / **850 MSPS/ch** (total 3,401 MSPS), 4ch×100 MSPS 0% drops ✅
- 10 MSPS: 0% drops ✅ (Phase 9 受入条件 PASS、全環境)
- ボトルネック: syscall overhead (~4.5 μs/frame)、総スループットはチャネル数非依存 (~3,400 MSPS)
- 詳細: `doc/TI-phase9.1.md`

---

## Phase 10: grebe-bench ベンチマークスイート (NFR-01, NFR-02)

NFR 検証用の独立ベンチマークツール。

- [ ] PoC BM-A/B/C/E の libgrebe API ベース移行
- [ ] NFR-01 (描画性能 L0–L3) の自動検証
- [ ] NFR-02 (E2E レイテンシ) の自動検証
- [ ] JSON レポート出力

**受入条件:**
- grebe-bench 単体実行で NFR-01/02 の合否判定が自動出力されること
- L2 (1ch × 1 GSPS ≥ 60 FPS) PASS

---

## Phase 11: クロスプラットフォーム + パッケージング (NFR-06)

Windows MSVC ビルドと CMake パッケージ配布。

- [ ] Windows MSVC ビルド検証 (libgrebe + grebe-viewer + grebe-bench)
- [ ] Linux/Windows 性能差 ≤ 20% の確認
- [ ] `find_package(grebe)` で利用可能な CMake config 生成
- [ ] doc/API.md (公開 API リファレンス)

**受入条件:**
- Linux (GCC) と Windows (MSVC) で同一ソースからビルド・動作すること
- 外部プロジェクトから `find_package(grebe)` で libgrebe をリンクできること

---

## 達成状況サマリ

| Phase | 内容 | ステータス |
|-------|------|-----------|
| 1. プロジェクト構造再編 | libgrebe + apps 分離 | ✅ 完了 |
| 2. IDataSource 抽象化 | FR-01 | ✅ 完了 |
| 3. IRenderBackend 抽象化 | FR-04 | ✅ 完了 |
| 4. DecimationEngine API | FR-02, FR-03 | ✅ 完了 |
| 5. Config + Telemetry | FR-05, FR-07 | ✅ 完了 |
| 6. libgrebe デカップリング | HUD/ImGui 除去 | ✅ 完了 |
| 7. データパイプライン純化 | Vulkan/Benchmark/不要依存の除去 | ✅ 完了 |
| 7.1 性能回帰検証 | R-1〜R-6 全 PASS | ✅ 完了 |
| 7.2 IPC トランスポート除去 | IpcSource/pipe_transport → apps/ | ✅ 完了 |
| 8. FileSource | FR-01 | ✅ 完了 |
| 9. UDP トランスポート | FR-01 | ✅ 完了 |
| 9.1 UDP ボトルネック調査 | BM-H ベンチマーク | ✅ 完了 |
| 10. grebe-bench | NFR-01, NFR-02 | 未着手 |
| 11. クロスプラットフォーム | NFR-06 | 未着手 |
