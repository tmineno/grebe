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

## Phase 7 (次回): libgrebe をデータパイプラインに純化

**設計方針:** libgrebe の本質的価値はデータパイプライン (取り込み → デシメーション → 出力) であり、
描画・計測・ロギングは本来アプリの責務。ライブラリを純粋なデータパイプラインに絞る。

### 7a. Vulkan 描画一式の除去

- [ ] Vulkan 描画モジュールを `apps/viewer/` に移動:
  - `vulkan_context.h/cpp`, `swapchain.h/cpp`, `renderer.h/cpp`
  - `buffer_manager.h/cpp`, `vulkan_renderer.h/cpp`
  - `compute_decimator.h/cpp`, `vma_impl.cpp`
- [ ] libgrebe のリンク依存から除去: Vulkan, GLFW, vk-bootstrap, VMA, glm, stb_headers
- [ ] `render_backend.h` (IRenderBackend インタフェイス定義) はライブラリに残すか検討
  - データパイプライン自体は描画を知らない → 残す意味は薄い
  - ただしヘッダのみでリンク依存なし → 残してもコストゼロ

### 7b. Benchmark (テレメトリ計測) の除去

- [ ] `benchmark.h/cpp` を `apps/viewer/` に移動
  - ライブラリ内で自己参照のみ (`benchmark.cpp` → `benchmark.h`)、外部消費は全て `apps/viewer/`
  - フレーム単位のローリング平均、CSV ロギングはアプリの計測関心事
  - `TelemetrySnapshot` 構造体は `include/grebe/telemetry.h` に残す (データ定義のみ)

### 7c. 不要なリンク依存の除去

- [ ] `nlohmann_json::nlohmann_json` を libgrebe から除去 (src/ 内で使用箇所なし — profiler/microbench 移動済み)
- [ ] `stb_headers` を libgrebe から除去 (src/ 内で使用箇所なし)

### 7d. Phase 7 完了後の libgrebe 構成

**src/ に残るファイル (データパイプラインコア):**
```
ring_buffer.h, ring_buffer_view.h     — SPSC ロックフリーキュー
decimator.h/cpp                       — MinMax (SSE2 SIMD), LTTB アルゴリズム
decimation_thread.h/cpp               — バックグラウンドデシメーションワーカー
decimation_engine.cpp                 — 公開ファサード
data_generator.h/cpp                  — 周期タイリング波形生成
synthetic_source.h/cpp                — IDataSource 実装 (組み込み波形)
ipc_source.h/cpp                      — IDataSource 実装 (IPC パイプ)
ingestion_thread.h/cpp                — DataSource → RingBuffer ドライバ
ipc/transport.h, pipe_transport.h/cpp — プラットフォーム IPC 実装
ipc/contracts.h                       — フレームヘッダ・コマンド構造体
drop_counter.h                        — ドロップカウンタ
waveform_type.h, waveform_utils.h     — 波形ユーティリティ
```

**リンク依存 (最小):**
```cmake
target_link_libraries(grebe PUBLIC spdlog::spdlog)  # ロギングのみ
```

**受入条件:**
- libgrebe が Vulkan/GLFW/ImGui/JSON に非依存でビルドできること
- `nm -C libgrebe.a | grep -c "Vulkan\|vk\|Swapchain\|Benchmark"` → 0
- grebe-viewer が libgrebe + Vulkan 描画コードで既存全モード動作を維持すること

---

## Phase 8: FileSource (FR-01)

バイナリファイル再生用の IDataSource 実装。

- [ ] FileSource 実装 (mmap + madvise(SEQUENTIAL))
- [ ] サンプルレートに応じたペーシング
- [ ] grebe-viewer からの FileSource 利用パス

**受入条件:**
- 事前キャプチャ済みバイナリファイルを指定して波形再生できること
- 1ch × 100 MSPS 相当のファイル再生で 60 FPS 動作

---

## Phase 9: UdpSource + grebe-udp-sender (FR-01)

UDP ソケット経由のデータ受信参照実装とデモ送信プログラム。

- [ ] UdpSource 実装 (POSIX ソケット / Winsock、外部依存なし)
- [ ] grebe-udp-sender デモプログラム (SyntheticSource 相当の波形を UDP 送信)
- [ ] ローカルループバック (127.0.0.1) での E2E パイプライン検証

**受入条件:**
- grebe-udp-sender → UdpSource → grebe-viewer でリアルタイム波形描画が動作すること
- ループバックで 1ch × 10 MSPS 以上のストリーミングが安定動作 (0 drops)

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
| 7. データパイプライン純化 | Vulkan/Benchmark/不要依存の除去 | 未着手 |
| 8. FileSource | FR-01 | 未着手 |
| 9. UdpSource | FR-01 | 未着手 |
| 10. grebe-bench | NFR-01, NFR-02 | 未着手 |
| 11. クロスプラットフォーム | NFR-06 | 未着手 |
