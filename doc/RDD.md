# Grebe — Vulkan 高速時系列ストリーム描画 PoC/MVP 要件定義書

**バージョン:** 1.0.0
**最終更新:** 2026-02-07
**ステータス:** Phase 0-7 完了

---

## 1. 目的と背景

### 1.1 目的

Vulkan を用いた時系列データストリームの高速描画パイプラインの技術的限界を検証する PoC/MVP を構築する。最終的なゴールはリアルタイム信号可視化基盤の実現可能性評価であり、本PoCではパイプライン各段のスループット・レイテンシを定量的に計測し、ボトルネックを特定する。

### 1.2 背景・動機

- 16bit/1GSPS クラスの高速 ADC データの可視化需要
- 既存ライブラリ（ImPlot, PyQtGraph 等）の性能上限を超える描画レートの探索
- CPU↔GPU 間データ転送、GPU 描画、間引きアルゴリズムそれぞれのスループット上限の把握

### 1.3 スコープ

**Phase 0-4 で実装済み:**
- Vulkan 描画パイプライン（データ転送→描画→表示）
- 合成データ生成（1ch, 最大 1 GSPS, period tiling による高速生成）
- MinMax (SSE2 SIMD) / LTTB 間引きアルゴリズム
- GPU Compute Shader 間引き実験 (TI-03)
- パイプライン各段の計測・プロファイリング基盤
- 独立マイクロベンチマーク (BM-A, B, C, E)
- ウィンドウ/UI（波形表示 + ImGui メトリクス HUD）
- ボトルネック分析レポート・技術的判断メモ

**Phase 5 で実装予定:**
- 複数チャンネル同時表示（4ch/8ch）
- LTTB の高レート自動無効化

**本PoCに含まないもの:**
- 実デバイス（ADC/FPGA）との接続
- 保存・再生機能
- 本格的な UI/UX（メニュー、設定画面等）
- ネットワーク経由のデータ受信

---

## 2. 前提条件

### 2.1 ターゲットデータ仕様

| 項目 | 値 | 備考 |
|---|---|---|
| サンプリングレート | 1 GSPS (10⁹ samples/sec) | 1ch 計測済み達成 |
| 量子化ビット数 | 16 bit (int16_t) | 2 bytes/sample |
| データレート | 2 GB/s (1ch) | 1G × 2 bytes |
| チャンネル数 | 1 (Phase 0-4) / 4-8 (Phase 5) | |
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
┌─────────────────────────────────────────────────────────────────────┐
│                        メインプロセス                                │
│                                                                     │
│  [データ生成スレッド]                                                 │
│       │ Period tiling (memcpy) ≥100 MSPS / LUT <100 MSPS           │
│       │                                                             │
│       │ lock-free SPSC ring buffer (CPU memory, 16M-64M samples)   │
│       ▼                                                             │
│  [間引きスレッド]                                                     │
│       │ MinMax (SSE2 SIMD) / LTTB → 3840 頂点/フレーム              │
│       │                                                             │
│       │ double buffer swap (mutex)                                  │
│       ▼                                                             │
│  [描画スレッド (メイン)]                                               │
│       │                                                             │
│       ├── Staging Buffer 書き込み (triple-buffered)                  │
│       ├── vkCmdCopyBuffer → Device Local Buffer                     │
│       ├── vkCmdDraw (LINE_STRIP, int16 vertex format)               │
│       ├── ImGui HUD オーバーレイ                                     │
│       └── vkQueuePresent                                            │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

### 3.2 コンポーネント構成

```
vulkan-stream-poc/
├── src/
│   ├── main.cpp                  # エントリポイント、CLI解析、メインループ
│   ├── vulkan_context.h/cpp      # Vulkan 初期化・デバイス管理
│   ├── swapchain.h/cpp           # スワップチェーン管理
│   ├── renderer.h/cpp            # 描画パイプライン (LINE_STRIP)
│   ├── buffer_manager.h/cpp      # Triple-buffered Staging/Device バッファ管理
│   ├── data_generator.h/cpp      # 合成データ生成 (period tiling + LUT)
│   ├── decimator.h/cpp           # 間引きアルゴリズム (MinMax SIMD / LTTB)
│   ├── decimation_thread.h/cpp   # 間引きワーカースレッド + ダブルバッファ出力
│   ├── ring_buffer.h             # lock-free SPSC ring buffer
│   ├── benchmark.h/cpp           # テレメトリ収集・ローリング平均・CSV出力
│   ├── hud.h/cpp                 # ImGui メトリクス HUD
│   ├── profiler.h/cpp            # 自動プロファイリングフレームワーク
│   ├── microbench.h/cpp          # 独立マイクロベンチマーク (BM-A/B/C/E)
│   ├── compute_decimator.h/cpp   # GPU Compute Shader 間引き (TI-03 実験)
│   └── vma_impl.cpp              # Vulkan Memory Allocator 実装
├── shaders/
│   ├── waveform.vert             # 頂点シェーダ (int16 → NDC)
│   ├── waveform.frag             # フラグメントシェーダ
│   └── minmax_decimate.comp      # GPU Compute MinMax シェーダ
├── doc/
│   ├── RDD.md                    # 本書
│   ├── bottleneck_analysis.md    # ボトルネック分析レポート
│   └── technical_judgment.md     # TI-01〜06 技術的判断メモ
├── CMakeLists.txt
└── README.md
```

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
- **FR-01.5:** ≥100 MSPS では period tiling (memcpy) による高速生成を行う（1 GSPS 達成済み）

### FR-02: データ間引き

- **FR-02.1:** MinMax decimation を実装する（各ピクセル幅に対して min/max ペアを出力、SSE2 SIMD 最適化）
- **FR-02.2:** LTTB (Largest Triangle Three Buckets) を実装する
- **FR-02.3:** 間引き無し（raw データ直接描画）モードを提供する（低レートベンチマーク用）
- **FR-02.4:** 間引き率 = (入力サンプル数) / (出力サンプル数) を動的に変更可能にする
- **FR-02.5:** 間引き処理は描画スレッドとは別スレッドで実行する
- **FR-02.6 (Phase 5):** ≥100 MSPS では LTTB を自動無効化し、MinMax にフォールバックする

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
  - **BM-E:** GPU Compute Shader 間引きスループット（TI-03 実験）

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

### FR-07: マルチチャンネル表示 (Phase 5)

- **FR-07.1:** `--channels=N` (N=1,2,4,8) で起動時にチャンネル数を指定可能にする（デフォルト: 1）
- **FR-07.2:** 各チャンネルを画面内の独立した垂直レーンに描画する
- **FR-07.3:** 各チャンネルに固有の色を割り当てる（Ch0=緑, Ch1=黄, Ch2=シアン, Ch3=マゼンタ 等）
- **FR-07.4:** 各チャンネルで独立に間引き処理を行う
- **FR-07.5:** N=1 のとき従来と同一の動作を保証する（後方互換）

---

## 5. 非機能要件

### NFR-01: 性能目標（段階的）

| レベル | 入力レート | 間引き | 描画FPS | 判定 | 結果 |
|---|---|---|---|---|---|
| L0: 基本動作 | 1 MSPS | MinMax | 60 fps | 必須 | **PASS** (60.0 fps) |
| L1: 中速 | 100 MSPS | MinMax | 60 fps | 必須 | **PASS** (59.9 fps) |
| L2: 高速 | 1 GSPS | MinMax | 60 fps | 目標 | **PASS** (59.7 fps) |
| L3: 限界探索 | 1 GSPS | MinMax | V-Sync OFF 最大 | 計測のみ | **2,022 FPS** (BM-C, Native RTX 5080) |

※ L0-L2 計測環境: WSL2, llvmpipe (software Vulkan), AMD Ryzen 9 9950X3D
※ L3 計測環境 (Phase 6): WSL2, Mesa dzn (D3D12→Vulkan), RTX 5080 → 770 FPS
※ L3 計測環境 (Phase 7): Windows Native, NVIDIA Vulkan ドライバ, RTX 5080, MSVC Release → **2,022 FPS**

### NFR-01b: マルチチャンネル性能目標 (Phase 5)

| レベル | チャンネル数 | 入力レート/ch | 間引き | 描画FPS | 判定 | 結果 |
|---|---|---|---|---|---|---|
| MC-1 | 4 | 1 MSPS | MinMax | ≥30 fps | 必須 | **PASS** (60.1 fps) |
| MC-2 | 4 | 100 MSPS | MinMax | ≥30 fps | 目標 | **PASS** (60.0 fps) |
| MC-3 | 8 | 10 MSPS | MinMax | ≥30 fps | 目標 | **PASS** (60.0 fps) |

### NFR-02: レイテンシ

- データ生成からピクセル表示までの E2E レイテンシを計測する
- 目標: L1 で 50ms 以下、L2 で 100ms 以下
- 推定: ring buffer + ダブルバッファ + V-Sync の 3 フレーム分 ≈ 50ms

### NFR-03: メモリ使用量

- Ring buffer サイズ: デフォルト 16M samples (32 MB)、1 GSPS 時 64M+ 推奨
- GPU Staging Buffer: 3面 × 描画頂点数分（数十KB程度）
- VRAM 総使用量: 512 MB 以下

### NFR-04: 安定性

- 1時間連続実行でクラッシュ・メモリリーク・VRAM リークが発生しないこと
- Vulkan Validation Layer エラーが0件であること

### NFR-05: ビルド・ポータビリティ

- Windows (MSVC 2022+) と Linux (GCC 12+ / Clang 15+) でビルド可能
- 外部依存は CMake FetchContent で解決
- GPU ベンダー非依存（NVIDIA / AMD / Intel で動作）

---

## 6. 技術的検証項目

PoCを通じて以下の技術的疑問に回答を得た。詳細は `doc/technical_judgment.md` を参照。

| TI | 項目 | 結論 |
|---|---|---|
| TI-01 | GPU 転送ボトルネック | 間引き後 7.68 KB/frame — **ボトルネックではない** (実GPU検証済み) |
| TI-02 | CPU 間引き性能 | MinMax SIMD 21,521 MSPS (Release, 1 GSPS に 21x マージン)。**LTTB は ≥100 MSPS で不適** |
| TI-03 | GPU 側間引き | CPU SIMD が 7.1x 高速 (dzn/RTX 5080)。**CPU 側間引き + 小データ転送が最適** |
| TI-04 | 描画プリミティブ | LINE_STRIP で 3840 vtx → 2,022 FPS (34x 余裕, Native RTX 5080)。**十分** |
| TI-05 | 永続マップドバッファ | **検証不可** (dzn ドライバは ReBAR 非対応)。現行設計で優先度低 |
| TI-06 | スレッドモデル | 3 スレッド + lock-free SPSC が最適。ring_fill <0.3% @ 1 GSPS |

---

## 7. 開発フェーズ

### Phase 0: 骨格 ✅

- Vulkan 初期化、スワップチェーン、GLFW ウィンドウ
- 固定データ（静的正弦波配列）の描画確認
- Vertex Shader（int16 → NDC 変換）
- フレームレート表示

### Phase 1: ストリーミング基盤 ✅

- データ生成スレッド + lock-free SPSC ring buffer
- Staging Buffer → Device Buffer 非同期転送
- Triple Buffering 実装
- 1 MSPS でのリアルタイム描画確認 (L0 達成)
- ImGui HUD、CSV テレメトリ

### Phase 2: 間引き実装 ✅

- MinMax decimation (scalar → SSE2 SIMD)
- LTTB 実装
- 間引きスレッド分離 + ダブルバッファ出力
- 100 MSPS でのリアルタイム描画確認 (L1 達成)

### Phase 3: ベンチマーク・最適化 ✅

- マイクロベンチマーク (BM-A, B, C, E) 実装
- 自動プロファイリングフレームワーク (`--profile`)
- Period tiling による 1 GSPS データ生成
- GPU Compute Shader 間引き実験 (TI-03)
- 1 GSPS リアルタイム描画達成 (L2 達成: 59.7 fps)

### Phase 4: 計測・文書化 ✅

- ボトルネック分析レポート (`doc/bottleneck_analysis.md`)
- 技術的判断メモ TI-01〜06 (`doc/technical_judgment.md`)
- 全レートで 60 FPS 達成、V-Sync 律速を確認

### Phase 5: マルチチャンネル + 高レート安全策 ✅

- `--channels=N` (N=1,2,4,8) CLIオプション
- チャンネルごとの独立リングバッファ・間引き・描画
- Per-channel カラーパレット + 垂直レーンレイアウト
- ≥100 MSPS での LTTB 自動無効化
- 受入条件: 4ch × 100 MSPS で ≥30 FPS

### Phase 6: 実 GPU 環境再計測 ✅

Mesa dzn ドライバ (D3D12→Vulkan 変換層) を WSL2 上にインストールし、NVIDIA GeForce RTX 5080 を Vulkan デバイスとして使用可能にした。BM-A/C/E マイクロベンチマーク、NFR-01 L3、NFR-01b MC プロファイルを実 GPU 上で計測。

- **計測環境**: WSL2, Mesa dzn (ppa:kisak/turtle), RTX 5080 (D3D12→Vulkan), Release ビルド
- **L3 計測**: 770 FPS (BM-C, V-Sync OFF, 3840 vtx)
- **TI-01**: BM-A 実 GPU 転送 4.8-24.4 GB/s → 間引き後 7.68 KB では無関係
- **TI-03**: GPU Compute 3,032 MSPS < CPU SIMD 21,521 MSPS → CPU 間引き最適を再確認
- **TI-05**: dzn は ReBAR/SAM 非対応のため未検証。ネイティブ GPU ドライバ環境が必要
- **全プロファイル PASS**: 1ch/4ch/8ch × 全レート (1M-1G SPS) で ≥30 FPS

### Phase 7: Windows ネイティブ MSVC ビルド ✅

WSL2 から MSVC (Visual Studio 2022) を呼び出して Windows ネイティブ .exe をビルドし、NVIDIA ネイティブ Vulkan ドライバで実行する環境を構築。dzn (D3D12→Vulkan 変換層) のオーバーヘッドを排除し、真の GPU 性能を計測。

- **ビルド環境**: WSL2 → MSVC 19.44 + Ninja + Vulkan SDK 1.4.341.1
- **依存インストール**: winget で Vulkan SDK, Git for Windows を自動インストール (`scripts/setup-windows.sh`)
- **ビルド自動化**: rsync + 生成 .bat で vcvarsall.bat x64 → cmake → ninja (`scripts/build-windows.sh`)
- **実行ヘルパー**: WSL2 から .exe を起動、結果ファイルを WSL プロジェクトにコピーバック (`scripts/run-windows.sh`)
- **L3 更新**: **2,022 FPS** (BM-C, 3840 vtx) — dzn 770 FPS から **2.6x 向上**
- **BM-C 全頂点数**: 3840→2,022 FPS / 38400→3,909 FPS / 384000→3,741 FPS
- **BM-A 転送**: ネイティブドライバで小バッファ転送が改善 (1MB: 10.3 vs 4.8 GB/s)
- **BM-E Compute**: 5,127 MSPS (dzn 3,032 → 1.7x 向上)
- **MSVC SIMD 特性**: Scalar 1,884 MSPS vs SIMD 19,834 MSPS = **10.5x** (GCC は auto-vectorize で 1.1x)
- **全プロファイル PASS**: 1ch × 全レート (1M-1G SPS) で 60 FPS 安定

---

## 8. 操作仕様

PoCのため GUI は最小限とし、キーボード操作を主体とする。

### キーボード操作

| キー | 操作 |
|---|---|
| `1` - `4` | データレート切替 (1M / 10M / 100M / 1G SPS) |
| `D` | 間引きアルゴリズム切替 (None / MinMax / LTTB) |
| `V` | V-Sync ON/OFF トグル |
| `Space` | 一時停止 / 再開 |
| `Esc` | 終了 |

### CLI オプション

| オプション | 説明 |
|---|---|
| `--log` | CSV テレメトリを `./tmp/` に出力 |
| `--profile` | 自動プロファイリング実行、JSON レポート出力 |
| `--bench` | 独立マイクロベンチマーク実行、JSON 出力 |
| `--ring-size=<N>[K\|M\|G]` | Ring buffer サイズ指定（デフォルト 16M） |
| `--channels=N` | チャンネル数指定（Phase 5, デフォルト 1） |

---

## 9. 成果物

| 成果物 | 形式 | 説明 | ステータス |
|---|---|---|---|
| ソースコード | C++ / CMake | ビルド可能なPoC一式 | 完了 |
| プロファイルレポート | JSON | 4シナリオの自動計測結果 | 完了 |
| マイクロベンチマーク結果 | JSON | BM-A/B/C/E の計測値 | 完了 |
| テレメトリログ | CSV | フレーム単位の詳細計測値 | 完了 |
| ボトルネック分析レポート | Markdown | パイプライン各段の律速要因分析 | 完了 |
| 技術的判断メモ | Markdown | TI-01〜06 への回答と推奨事項 | 完了 |

---

## 10. リスクと制約

| リスク | 影響 | 結果 |
|---|---|---|
| 1GSPS の CPU 間引きが追いつかない | L2 未達 | **緩和済み**: MinMax SIMD 1,526 MSPS で 1.5x マージン確保 |
| PCIe 帯域が律速 | L2 未達 | **設計で回避**: 間引き後 7.68 KB/frame のみ転送 |
| Vulkan 初期化の複雑さ | 全体遅延 | **緩和済み**: vk-bootstrap 活用 |
| GPU ベンダー間の挙動差 | 再現性低下 | **検証済み**: RTX 5080 で dzn (Phase 6) + ネイティブ NVIDIA Vulkan (Phase 7) の両方で動作確認 |
| LTTB が高レートで追いつかない | 描画落ち | **Phase 5 で対策**: ≥100 MSPS で自動無効化 |

### 制約事項

- Phase 0-5 の計測は llvmpipe (software Vulkan, Debug ビルド) 環境
- Phase 6 で RTX 5080 (dzn, Release ビルド) を追加計測。dzn は D3D12→Vulkan 変換層であり、ネイティブ Vulkan ドライバとは性能特性が異なる
- Phase 7 で Windows ネイティブ NVIDIA Vulkan ドライバ (MSVC Release) による計測を実施。dzn 比 2.6x の描画性能向上を確認

---

## 11. 将来拡張（PoC 後の検討事項）

Phase 7 以降で検討が必要な項目:

### 優先度中（`doc/technical_judgment.md` より）

- AVX2 MinMax 最適化（SSE2 → AVX2 で処理幅 8→16 に倍増）

### 優先度低

- E2E レイテンシ計測（NFR-02 目標の検証）
- マルチスレッド並列間引き（2 GSPS+ 向け）
- 太線描画（Instanced Quad）

### 製品化時

- 実デバイス接続（PCIe DMA / USB3 / 10GbE）
- ウォーターフォール / スペクトログラム表示
- トリガ機能（レベル/エッジ/パターントリガ）
- データ録画・再生
- GUI フレームワーク統合（Qt / ImGui 本格UI）
- リモート表示（WebSocket 経由ブラウザ表示）
- GPU Direct (RDMA) によるデバイス→VRAM 直接転送

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

### マイクロベンチマーク (llvmpipe, WSL2, Debug ビルド)

| ベンチマーク | 結果 |
|---|---|
| BM-A: CPU→GPU 転送 | 11.4 (1MB) / 31.6 (4MB) / 35.9 (16MB) / 17.4 (64MB) GB/s |
| BM-B: MinMax Scalar | 1,354 MSPS |
| BM-B: MinMax SIMD (SSE2) | 1,526 MSPS |
| BM-B: LTTB | 213 MSPS |
| BM-C: 描画 (3840 vtx) | 470 FPS (2.1 ms/frame) |
| BM-E: GPU Compute MinMax | 472 MSPS |

### マイクロベンチマーク (RTX 5080 via dzn, WSL2, Release ビルド)

| ベンチマーク | 結果 | llvmpipe比 |
|---|---|---|
| BM-A: CPU→GPU 転送 | 4.8 (1MB) / 12.0 (4MB) / 20.1 (16MB) / 24.4 (64MB) GB/s | D3D12 変換オーバーヘッド |
| BM-B: MinMax Scalar | 19,988 MSPS | 14.8x (Release 最適化) |
| BM-B: MinMax SIMD (SSE2) | 21,521 MSPS | 14.1x (Release 最適化) |
| BM-B: LTTB | 734 MSPS | 3.4x (Release 最適化) |
| BM-C: 描画 (3840 vtx) | 770 FPS (1.3 ms/frame) | 1.6x |
| BM-C: 描画 (38400 vtx) | 797 FPS (1.3 ms/frame) | 2.8x |
| BM-C: 描画 (384000 vtx) | 838 FPS (1.2 ms/frame) | 8.3x |
| BM-E: GPU Compute MinMax | 3,032 MSPS | 6.4x |

※ BM-B (CPU) の差は Release vs Debug ビルドの最適化差であり、GPU 変更の影響ではない
※ BM-A は llvmpipe が CPU→CPU memcpy、dzn が D3D12 経由のため単純比較不可

### マイクロベンチマーク (RTX 5080 Native NVIDIA Vulkan, Windows, MSVC Release)

| ベンチマーク | 結果 | dzn比 |
|---|---|---|
| BM-A: CPU→GPU 転送 | 10.3 (1MB) / 18.3 (4MB) / 22.2 (16MB) / 23.7 (64MB) GB/s | 小バッファ 2.2x 改善 |
| BM-B: MinMax Scalar | 1,884 MSPS | MSVC auto-vectorize 無し |
| BM-B: MinMax SIMD (SSE2) | 19,834 MSPS | ~同等 (GCC vs MSVC) |
| BM-B: LTTB | 743 MSPS | ~同等 |
| BM-B: SIMD 高速化率 | **10.5x** | GCC は 1.1x (auto-vectorize のため) |
| BM-C: 描画 (3840 vtx) | **2,022 FPS** (0.49 ms/frame) | **2.6x** |
| BM-C: 描画 (38400 vtx) | **3,909 FPS** (0.26 ms/frame) | **4.9x** |
| BM-C: 描画 (384000 vtx) | **3,741 FPS** (0.27 ms/frame) | **4.5x** |
| BM-E: GPU Compute MinMax | **5,127 MSPS** (3.2 ms/call) | **1.7x** |

※ BM-B の Scalar 差は MSVC が auto-vectorize しないため。SIMD 実装の効果がより明確に測定可能
※ BM-C で dzn 比 2.6-4.9x の改善は D3D12 変換オーバーヘッド排除による効果

### プロファイル結果 (RTX 5080 via dzn, Release ビルド)

**1ch:**

| シナリオ | FPS avg | FPS min | Frame ms | Render ms |
|---|---|---|---|---|
| 1MSPS | 60.0 | 58.7 | 16.66 | 16.15 |
| 10MSPS | 60.0 | 57.2 | 16.66 | 16.18 |
| 100MSPS | 60.1 | 58.3 | 16.65 | 16.16 |
| 1GSPS | 60.0 | 58.1 | 16.66 | 16.13 |

**4ch:**

| シナリオ | FPS avg | FPS min | Frame ms | Dec ms |
|---|---|---|---|---|
| 4ch×1MSPS | 60.0 | 57.7 | 16.66 | 0.04 |
| 4ch×10MSPS | 60.1 | 58.6 | 16.65 | 0.02 |
| 4ch×100MSPS | 60.0 | 57.9 | 16.66 | 0.03 |
| 4ch×1GSPS | 56.4 | 53.9 | 17.75 | ~18 |

**8ch:**

| シナリオ | FPS avg | FPS min | Frame ms | Dec ms |
|---|---|---|---|---|
| 8ch×1MSPS | 60.1 | 58.0 | 16.65 | 0.05 |
| 8ch×10MSPS | 60.0 | 57.4 | 16.66 | 0.05 |
| 8ch×100MSPS | 59.9 | 57.9 | 16.69 | 0.06 |
| 8ch×1GSPS | 56.4 | 53.2 | 17.75 | ~45 |

### プロファイル結果 (RTX 5080 Native NVIDIA Vulkan, MSVC Release)

**1ch:**

| シナリオ | FPS avg | FPS min | Frame ms | Render ms |
|---|---|---|---|---|
| 1MSPS | 60.3 | 48.1 | 16.64 | 11.00 |
| 10MSPS | 60.2 | 48.9 | 16.64 | 11.07 |
| 100MSPS | 60.2 | 52.3 | 16.63 | 10.95 |
| 1GSPS | 60.2 | 52.8 | 16.62 | 11.11 |

※ dzn 比で render_ms が 16ms → 11ms に改善 (D3D12 変換オーバーヘッド排除)
※ FPS は V-Sync 律速で ~60 FPS で横並び

詳細は `doc/bottleneck_analysis.md` および `doc/technical_judgment.md` を参照。
