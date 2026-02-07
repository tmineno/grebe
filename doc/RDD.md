# Grebe — Vulkan 高速時系列ストリーム描画 PoC/MVP 要件定義書

**バージョン:** 0.1.0-draft
**最終更新:** 2026-02-07
**ステータス:** ドラフト

---

## 1. 目的と背景

### 1.1 目的

Vulkan を用いた時系列データストリームの高速描画パイプラインの技術的限界を検証する PoC/MVP を構築する。最終的なゴールはリアルタイム信号可視化基盤の実現可能性評価であり、本PoCではパイプライン各段のスループット・レイテンシを定量的に計測し、ボトルネックを特定する。

### 1.2 背景・動機

- 16bit/1GSPS クラスの高速 ADC データの可視化需要
- 既存ライブラリ（ImPlot, PyQtGraph 等）の性能上限を超える描画レートの探索
- CPU↔GPU 間データ転送、GPU 描画、間引きアルゴリズムそれぞれのスループット上限の把握

### 1.3 スコープ

**本PoCに含むもの:**
- Vulkan 描画パイプライン（データ転送→描画→表示）
- 合成データによるベンチマーク機構
- 段階的な間引きアルゴリズム実装
- パイプライン各段の計測・プロファイリング基盤
- 最小限のウィンドウ/UI（波形表示 + メトリクス表示）

**本PoCに含まないもの:**
- 実デバイス（ADC/FPGA）との接続
- 複数チャンネル同時表示（将来拡張）
- 保存・再生機能
- 本格的な UI/UX（メニュー、設定画面等）
- ネットワーク経由のデータ受信

---

## 2. 前提条件

### 2.1 ターゲットデータ仕様

| 項目 | 値 | 備考 |
|---|---|---|
| サンプリングレート | 1 GSPS (10⁹ samples/sec) | ベンチマーク上限目標 |
| 量子化ビット数 | 16 bit (int16_t) | 2 bytes/sample |
| データレート | 2 GB/s | 1G × 2 bytes |
| チャンネル数 | 1 (PoC) | 将来拡張で複数対応 |
| データ形式 | リトルエンディアン int16 連続配列 | メモリ上のバイナリ列 |

### 2.2 ターゲット環境

| 項目 | 要件 |
|---|---|
| OS | Windows 10/11, Linux (Ubuntu 22.04+) |
| GPU | Vulkan 1.2 以上対応 (離散GPU推奨) |
| VRAM | 4 GB 以上 |
| RAM | 16 GB 以上 |
| CPU | x86_64, 4コア以上 |

### 2.3 言語・ツールチェーン

| 項目 | 選定 | 理由 |
|---|---|---|
| 言語 | C++ (C++20) | Vulkan API との親和性、最大性能 |
| ビルドシステム | CMake 3.24+ | Win/Linux クロスプラットフォーム |
| Vulkan SDK | LunarG Vulkan SDK 1.3+ | バリデーションレイヤー、プロファイラ含む |
| ウィンドウ | GLFW 3.3+ | 軽量、Vulkan surface 生成対応 |
| シェーダ | GLSL → SPIR-V (glslc) | 標準ツールチェーン |
| プロファイリング | Vulkan timestamp queries + CPU chrono | パイプライン各段の計測 |

---

## 3. アーキテクチャ概要

### 3.1 パイプライン全体像

```
┌─────────────────────────────────────────────────────────────────────┐
│                        メインプロセス                                │
│                                                                     │
│  [データ生成スレッド]                                                 │
│       │                                                             │
│       │ lock-free ring buffer (CPU memory)                          │
│       ▼                                                             │
│  [間引きスレッド]                                                     │
│       │ MinMax / LTTB → 描画用バッファ                                │
│       │                                                             │
│       │ double/triple buffer swap                                   │
│       ▼                                                             │
│  [描画スレッド (メイン)]                                               │
│       │                                                             │
│       ├── Staging Buffer 書き込み                                    │
│       ├── vkCmdCopyBuffer → Device Local Buffer                     │
│       ├── vkCmdDraw (Line Strip)                                    │
│       ├── メトリクス HUD オーバーレイ                                  │
│       └── vkQueuePresent                                            │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

### 3.2 コンポーネント構成

```
vulkan-stream-poc/
├── src/
│   ├── main.cpp                  # エントリポイント、GLFW初期化
│   ├── vulkan_context.h/cpp      # Vulkan 初期化・デバイス管理
│   ├── swapchain.h/cpp           # スワップチェーン管理
│   ├── renderer.h/cpp            # 描画パイプライン
│   ├── buffer_manager.h/cpp      # Staging/Device バッファ管理
│   ├── data_generator.h/cpp      # 合成データ生成
│   ├── decimator.h/cpp           # 間引きアルゴリズム
│   ├── ring_buffer.h             # lock-free ring buffer
│   ├── benchmark.h/cpp           # 計測・統計収集
│   └── hud.h/cpp                 # メトリクス表示 (テキスト/数値)
├── shaders/
│   ├── waveform.vert             # 頂点シェーダ
│   └── waveform.frag             # フラグメントシェーダ
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
- **FR-01.4:** 生成レートを段階的に変更可能にする（ベンチマーク用: 1MSPS → 10MSPS → 100MSPS → 1GSPS）

### FR-02: データ間引き

- **FR-02.1:** MinMax decimation を実装する（各ピクセル幅に対して min/max ペアを出力）
- **FR-02.2:** LTTB (Largest Triangle Three Buckets) を実装する
- **FR-02.3:** 間引き無し（raw データ直接描画）モードを提供する（低レートベンチマーク用）
- **FR-02.4:** 間引き率 = (入力サンプル数) / (出力サンプル数) を動的に変更可能にする
- **FR-02.5:** 間引き処理は描画スレッドとは別スレッドで実行する

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
- **FR-04.3:** 画面上にリアルタイムメトリクスを HUD 表示する（FR-06 参照）

### FR-05: ベンチマークモード

- **FR-05.1:** 自動ベンチマークシーケンスを実行可能にする
  - 段階的にデータレートを上げていき、各段階で一定時間（例: 5秒）計測
  - 各段階のメトリクスを記録
- **FR-05.2:** ベンチマーク結果を CSV/JSON ファイルに出力する
- **FR-05.3:** 以下の独立したマイクロベンチマークを提供する
  - **BM-A:** CPU→GPU 転送スループット（間引き・描画なし、vkCmdCopyBuffer のみ）
  - **BM-B:** 間引きスループット（CPU のみ、各アルゴリズム単体）
  - **BM-C:** 描画スループット（固定データ、転送なし、描画のみ）
  - **BM-D:** E2E（生成→間引き→転送→描画→表示）

### FR-06: メトリクス収集・表示

以下のメトリクスをリアルタイムで計測・表示する:

| メトリクス | 単位 | 説明 |
|---|---|---|
| FPS | frames/sec | 描画フレームレート |
| Frame Time | ms | 1フレームの所要時間（min/avg/max/p99） |
| GPU Transfer Rate | GB/s | Staging → Device 転送速度 |
| Decimation Rate | Msamples/s | 間引き処理速度 |
| Decimation Ratio | x:1 | 間引き率 |
| Input Data Rate | MSPS / GSPS | 入力データレート |
| Ring Buffer Fill | % | ring buffer の充填率 |
| GPU Memory Usage | MB | VRAM 使用量 |
| Draw Call Vertices | count | 1フレームの頂点数 |
| CPU Usage per Thread | % | 各スレッドの CPU 使用率 |

---

## 5. 非機能要件

### NFR-01: 性能目標（段階的）

本PoCの性能目標は「限界の発見」が主目的であるため、以下を段階的達成目標とする。

| レベル | 入力レート | 間引き | 描画FPS | 判定 |
|---|---|---|---|---|
| L0: 基本動作 | 1 MSPS | なし | 60 fps | 必須 |
| L1: 中速 | 100 MSPS | MinMax | 60 fps | 必須 |
| L2: 高速 | 1 GSPS | MinMax | 60 fps | 目標 |
| L3: 限界探索 | 1 GSPS | MinMax | V-Sync OFF 最大 | 計測のみ |

### NFR-02: レイテンシ

- データ生成からピクセル表示までの E2E レイテンシを計測する
- 目標: L1 で 50ms 以下、L2 で 100ms 以下（表示遅延として許容可能な範囲）
- ※ PoC 段階では目標値の達成より計測自体が重要

### NFR-03: メモリ使用量

- Ring buffer サイズ: 最大 2 GB（1秒分 @ 1GSPS × 2bytes）
- GPU Staging Buffer: 3面 × 描画頂点数分（数十MB程度）
- VRAM 総使用量: 512 MB 以下

### NFR-04: 安定性

- 1時間連続実行でクラッシュ・メモリリーク・VRAM リークが発生しないこと
- Vulkan Validation Layer エラーが0件であること

### NFR-05: ビルド・ポータビリティ

- Windows (MSVC 2022+) と Linux (GCC 12+ / Clang 15+) でビルド可能
- 外部依存は CMake FetchContent または vcpkg で解決
- GPU ベンダー非依存（NVIDIA / AMD / Intel で動作）

---

## 6. 技術的検証項目

PoCを通じて以下の技術的疑問に回答を得る。

### TI-01: GPU 転送ボトルネック

- PCIe 帯域（Gen3 x16: 理論 ~16 GB/s、Gen4: ~32 GB/s）に対して Vulkan vkCmdCopyBuffer の実効スループットはどこまで出るか
- Staging Buffer のサイズと転送頻度のトレードオフ（大きなバッチ vs 小さなバッチ頻繁転送）

### TI-02: CPU 間引き性能

- MinMax の SIMD (AVX2/SSE4) 最適化でどこまでスループットが出るか
- LTTB は 1GSPS に対して実用的な速度で動作するか
- マルチスレッド並列間引きの効果

### TI-03: GPU 側間引きの可能性

- Compute Shader で間引きを行い、描画パイプラインに直結した場合の性能差
- CPU→GPU 転送量を raw のまま送り GPU 側で間引く方が速いか、CPU で間引いてから小さいデータを送る方が速いか

### TI-04: 描画プリミティブ選択

- GL_LINE_STRIP 相当 vs Instanced Quad（太線）vs GL_POINTS の描画コスト比較
- 頂点数と描画パフォーマンスの関係（何頂点で描画が律速になるか）

### TI-05: 永続マップドバッファ

- `VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | DEVICE_LOCAL_BIT`（ReBAR/SAM対応環境）での直接書き込みは Staging 経由より速いか

### TI-06: スレッドモデル

- 生成・間引き・転送・描画の分離粒度とスレッド数の最適解
- lock-free ring buffer vs mutex-guarded double buffer のスループット差

---

## 7. 開発フェーズ

### Phase 0: 骨格 (目安: 2-3日)

- Vulkan 初期化、スワップチェーン、GLFW ウィンドウ
- 固定データ（静的正弦波配列）の描画確認
- 基本的な Vertex Shader（int16 → NDC 変換）
- フレームレート表示

### Phase 1: ストリーミング基盤 (目安: 3-4日)

- データ生成スレッド + ring buffer
- Staging Buffer → Device Buffer 非同期転送
- Triple Buffering 実装
- 1 MSPS でのリアルタイム描画確認 (L0 達成)

### Phase 2: 間引き実装 (目安: 2-3日)

- MinMax decimation 実装
- LTTB 実装
- 間引きスレッド分離
- 100 MSPS でのリアルタイム描画確認 (L1 達成)

### Phase 3: ベンチマーク・最適化 (目安: 3-5日)

- マイクロベンチマーク (BM-A, B, C, D) 実装
- 自動ベンチマークシーケンス
- SIMD 間引き最適化
- Compute Shader 間引き実験 (TI-03)
- 結果出力 (CSV/JSON)
- 1 GSPS 挑戦 (L2, L3)

### Phase 4: 計測・文書化 (目安: 1-2日)

- 複数環境での計測実施
- ボトルネック分析レポート
- 次ステップ（製品化判断）への推奨事項

**合計目安: 11-17日**

---

## 8. 操作仕様（最小限）

PoCのため GUI は最小限とし、キーボード操作を主体とする。

| キー | 操作 |
|---|---|
| `1` - `4` | データレート切替 (1M / 10M / 100M / 1G SPS) |
| `D` | 間引きアルゴリズム切替 (None / MinMax / LTTB) |
| `V` | V-Sync ON/OFF トグル |
| `B` | 自動ベンチマーク開始 |
| `H` | HUD 表示 ON/OFF |
| `+` / `-` | 時間軸ズーム |
| `↑` / `↓` | 振幅スケール |
| `Space` | 一時停止 / 再開 |
| `Esc` | 終了 |

---

## 9. 成果物

| 成果物 | 形式 | 説明 |
|---|---|---|
| ソースコード | C++ / CMake | ビルド可能なPoC一式 |
| ベンチマーク結果 | CSV + JSON | 各環境・各条件での計測値 |
| ボトルネック分析レポート | Markdown | 計測結果の考察、律速要因の分析 |
| 技術的判断メモ | Markdown | TI-01〜06 への回答と推奨事項 |

---

## 10. リスクと制約

| リスク | 影響 | 緩和策 |
|---|---|---|
| 1GSPS の CPU 間引きが追いつかない | L2 未達 | GPU Compute 間引きへの切替、またはデータ生成側でのプリデシメーション |
| PCIe 帯域が律速 | L2 未達 | ReBAR/SAM 直接書き込み検証、転送データ量削減（CPU側間引き優先） |
| Vulkan 初期化の複雑さで Phase 0 が長引く | 全体遅延 | vk-bootstrap 等の初期化ヘルパーライブラリ活用 |
| GPU ベンダー間の挙動差 | 再現性低下 | 複数環境での計測を Phase 4 に含める |

---

## 11. 将来拡張（PoC 後の検討事項）

本PoCでは対象外とするが、製品化時に検討が必要な項目:

- マルチチャンネル同時表示（4ch, 8ch, ...）
- ウォーターフォール / スペクトログラム表示
- 実デバイス接続（PCIe DMA / USB3 / 10GbE）
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
画面幅 1920px の場合、MinMax で 1920 × 2 = 3840 頂点/フレーム → **約 15 KB/フレーム**。
間引き後の GPU 転送量は全レートで事実上同一であり、間引き処理自体の CPU コストが律速となる。

## 付録B: 依存ライブラリ候補

| ライブラリ | 用途 | ライセンス |
|---|---|---|
| GLFW | ウィンドウ・入力 | Zlib |
| vk-bootstrap | Vulkan 初期化簡略化 | MIT |
| VMA (Vulkan Memory Allocator) | メモリ管理 | MIT |
| glm | 数学（行列・ベクトル） | MIT |
| stb_truetype | HUD テキスト描画 | Public Domain |
| nlohmann/json | ベンチマーク結果出力 | MIT |
| spdlog | ロギング | MIT |
