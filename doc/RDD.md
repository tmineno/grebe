# Grebe — 高速リアルタイム波形描画ライブラリ 要件定義書

**バージョン:** 2.1.0
**最終更新:** 2026-02-11

> **PoC (v0.1.0) からの移行**: 本ドキュメントは PoC フェーズ (Phase 0–13.5) の成果を踏まえ、プロジェクト目的を「任意のインタフェイスを用いた実デバイス入力をサポートする高速リアルタイム波形描画ライブラリ」に再定義したものである。PoC の技術検証結果は [TR-001](TR-001.md) を参照。
>
> **v2.1.0 更新**: Phase 1–7.2 のリファクタリング完了を反映。libgrebe を純粋データパイプラインに純化（Vulkan、ImGui、IPC、ベンチマーク等をアプリケーション側に分離）。アーキテクチャ図、API 定義、成果物一覧、移行マッピングを現在の実装に同期。

---

## 1. 目的と背景

### 1.1 目的

任意のデータ取得インタフェイス（NIC、USB3、PCIe 等）から入力される高速時系列データを、リアルタイムに波形描画するための C++ フレームワーク **libgrebe** と、その参照実装アプリケーション **grebe-viewer** を開発する。

### 1.2 背景

PoC (v0.1.0, TR-001) において以下が定量的に検証された：

| 検証項目 | 結果 | 参照 |
|----------|------|------|
| 1 GSPS 16-bit データの 60 FPS 描画 | 達成 (59.7 FPS) | TR-001 §概要 |
| CPU MinMax SIMD デシメーション | 19,834 MSPS | TR-001 §2.1 |
| CPU デシメーション > GPU コンピュート | 3.9–7.1× 高速 | TR-001 §3.1 |
| E2E レイテンシ p99 | 18.1 ms (目標 ≤50 ms) | TR-001 §5 |
| 波形忠実度（エンベロープ一致） | 100% (Embedded) | TR-001 §4 |
| マルチチャネル 4ch × 1 GSPS ゼロドロップ | 達成 | TR-001 §2.4 |

これらの実績を基盤に、合成データ生成器に依存しない汎用ライブラリへ再構築する。

### 1.3 PoC からの主要な設計変更

| 項目 | PoC (v0.1.0) | ライブラリ (v2.0) |
|------|-------------|-------------------|
| データ入力 | grebe-sg サブプロセス（合成波形） | IDataSource 抽象インタフェイス |
| 描画 | Vulkan + GLFW 直接結合 | IRenderBackend 抽象（ヘッダのみ）+ アプリ側 Vulkan 実装 |
| 構成管理 | CLI 引数 + グローバル状態 | PipelineConfig 構造体ベース |
| メトリクス | ImGui HUD 埋め込み | TelemetrySnapshot 構造体、計測・UI はアプリ側 |
| プロセスモデル | 2 プロセス必須 | シングルプロセス (Embedded) / IPC (grebe-sg) 両対応 |
| ライブラリ依存 | Vulkan, GLFW, ImGui, VMA 等多数 | spdlog のみ |

---

## 2. 前提条件

### 2.1 対象データ仕様

| 項目 | 仕様 |
|------|------|
| サンプル幅 | 16-bit 符号付き整数 (int16_t) |
| チャネル数 | 1–8 ch |
| 最大サンプルレート | 1 GSPS / ch |
| 最大合計スループット | 8 GSPS (8ch × 1 GSPS) = 16 GB/s |

### 2.2 入力インタフェイスと OS 転送メカニズム

ライブラリは以下のインタフェイスからのデータ入力を抽象化する。具体的なドライバ実装はライブラリのスコープ外とし、IDataSource インタフェイスを通じて結合する。

| インタフェイス | 想定用途 | 帯域目安 |
|---------------|----------|----------|
| PCIe (DMA) | FPGA 直結 ADC | 数 GB/s–数十 GB/s |
| USB 3.x | 汎用計測器 | 最大 5 Gbps (USB3.0) |
| NIC (Ethernet) | ネットワーク接続型計測器 | 1–100 GbE |
| ファイル再生 | キャプチャ済みデータの解析 | ストレージ速度依存 |
| 合成信号 | テスト・デモ用 | CPU 速度依存 |

#### OS レベルの転送メカニズム

高帯域データ転送に利用可能な OS メカニズムは、デバイス種別ごとに異なる。統一的な「OS 標準の広帯域転送 API」は存在せず、バッファ所有権モデル・通知モデル・フレーミングの 3 軸がそれぞれ異なるため、トランスポートレベルでの統一は不可能である。これが IDataSource による**フレーム配信レベルでの抽象化**を採用する根拠となる。

| メカニズム | OS | 対象デバイス | ゼロコピー | カーネルバイパス |
|-----------|-----|-------------|-----------|----------------|
| VFIO / UIO | Linux | PCIe デバイス全般 (FPGA, NIC, GPU) | ○ (DMA) | 完全 |
| AF_XDP | Linux 4.18+ | NIC (PCIe 接続) | ○ | 部分的 |
| DPDK | Linux | NIC (PCIe 接続) | ○ | 完全 |
| libusb (async bulk) | Linux/Windows/macOS | USB デバイス | △ | × |
| io_uring | Linux 5.1+ | ファイル / NIC / 汎用 | ○ (6.15+) | 部分的 |
| mmap | 全 OS | ファイル | ○ | N/A |
| IOCP + Registered I/O | Windows | ファイル / NIC / 汎用 | △ | × |

**デバイス種別と推奨メカニズムの対応:**

| デバイス種別 | バス | 推奨メカニズム | 備考 |
|-------------|------|---------------|------|
| FPGA / ADC カード | PCIe 直結 | VFIO + ベンダ DMA ドライバ (Xilinx XDMA, Intel OPAE 等) | ユーザ空間から DMA バッファに直接アクセス |
| NIC (高帯域) | PCIe 直結 | AF_XDP or DPDK (内部で vfio-pci) | Ethernet プロトコル処理込み |
| USB 計測器 | USB バス | libusb async bulk | USB プロトコルスタックが必須のため VFIO 不可 |
| ファイル再生 | ストレージ | mmap + madvise(SEQUENTIAL) or io_uring | NVMe SSD で 5–7 GB/s |

> **注記**: NIC は PCIe デバイスであるため VFIO で直接バインド可能だが、Ethernet/IP/UDP のプロトコル解析が必要なため、実用上は DPDK や AF_XDP といったプロトコル処理込みのフレームワークを介する。USB デバイスは PCIe バス上の USB ホストコントローラの配下にあり、USB プロトコルスタック（エニュメレーション、エンドポイント管理）が不可欠なため、VFIO による直接アクセスは不可能である。

#### 統一できない 3 軸

| 軸 | DPDK (NIC) | libusb (USB) | VFIO (PCIe DMA) | mmap (File) |
|----|-----------|-------------|-----------------|-------------|
| バッファ所有権 | mempool (ライブラリ管理) | ユーザ確保 | ユーザ確保 + IOMMU | カーネルページキャッシュ |
| 通知モデル | polling (busy-wait) | callback / blocking | eventfd / polling | 同期 / ページフォルト |
| フレーム単位 | Ethernet frame | USB bulk transfer | デバイス定義 | 生バイト列 |

### 2.3 動作環境

| 項目 | 要件 |
|------|------|
| OS | Linux (x86_64), Windows 10/11 (x86_64) |
| GPU API | Vulkan 1.0 以上 |
| コンパイラ | GCC 12+ / MSVC 19.30+ (C++20) |
| ビルドシステム | CMake 3.24+ |
| CPU 命令セット | SSE2 必須、AVX2 推奨 |

---

## 3. アーキテクチャ概要

### 3.1 コンポーネント構成

```
┌──────────────────────────────────────────────────────────────┐
│ アプリケーション (grebe-viewer, grebe-sg 等)                  │
│                                                              │
│  ┌───────────┐  ┌──────────────┐  ┌────────────────────┐    │
│  │ UI / HUD  │  │ DataSource   │  │ Telemetry          │    │
│  │ (ImGui等) │  │ 実装         │  │ Consumer           │    │
│  └─────┬─────┘  │ (IpcSource等)│  │ (Benchmark, CSV等) │    │
│        │        └──────┬───────┘  └─────────┬──────────┘    │
│        │               │                    │                │
│  ┌─────▼─────────────┐ │                    │                │
│  │ VulkanRenderer     │ │                    │                │
│  │ (IRenderBackend    │ │                    │                │
│  │  実装)             │ │                    │                │
│  └─────┬─────────────┘ │                    │                │
│        │               │                    │                │
├────────┼───────────────┼────────────────────┼────────────────┤
│ libgrebe (コアライブラリ — 純粋データパイプライン)             │
│        │               │                    │                │
│  ┌─────▼──────┐  ┌─────▼──────┐  ┌─────────▼──────────┐    │
│  │IRender     │  │ IDataSource│  │ PipelineConfig     │    │
│  │Backend     │  │ (abstract) │  │ TelemetrySnapshot  │    │
│  │(abstract,  │  │            │  │ (データ定義のみ)     │    │
│  │ header only│  └─────┬──────┘  └────────────────────┘    │
│  └────────────┘        │                                    │
│              ┌─────────▼──────────┐                         │
│              │ IngestionThread    │                         │
│              │ (DataSource→Ring)  │                         │
│              └─────────┬──────────┘                         │
│              ┌─────────▼──────────┐                         │
│              │ N × RingBuffer     │                         │
│              │ (lock-free SPSC)   │                         │
│              └─────────┬──────────┘                         │
│              ┌─────────▼──────────┐                         │
│              │ DecimationEngine   │                         │
│              │ (single worker     │                         │
│              │  thread, N ch)     │                         │
│              └────────────────────┘                         │
└──────────────────────────────────────────────────────────────┘
```

**libgrebe の責務は純粋なデータパイプライン（取り込み → リングバッファ → デシメーション → 出力）のみ。** 描画 (VulkanRenderer)、UI (HUD/ImGui)、テレメトリ計測 (Benchmark)、IPC トランスポート (PipeTransport) はすべてアプリケーション側に配置される。ライブラリは抽象インタフェイス定義（`IDataSource`, `IRenderBackend`）とデータ型定義（`TelemetrySnapshot`, `PipelineConfig`, `DrawCommand` 等）をヘッダとして提供する。

### 3.2 主要抽象インタフェイス

#### IDataSource

アプリケーションが実装し、libgrebe に注入するデータ供給インタフェイス。各デバイス固有の転送メカニズム（§2.2 参照）の差異を吸収し、libgrebe に対して統一的なフレームストリームを提供する。

```
デバイス固有の世界                    libgrebe の世界
┌─────────────────┐
│ DPDK            │ rx_burst → パケット解析
│ (NIC, polling)  │ → UDP デフレーム
│                 ├──────▶ FrameHeader + int16_t[]
├─────────────────┤            │
│ libusb          │        IDataSource
│ (USB, callback) ├──────▶ .read_frame()
├─────────────────┤            │
│ VFIO + XDMA     │            ▼
│ (PCIe DMA)      ├──────▶ RingBuffer → DecimationEngine
├─────────────────┤
│ mmap            │
│ (File)          ├──────▶
└─────────────────┘
```

```cpp
class IDataSource {
public:
    virtual ~IDataSource() = default;

    // データソースの情報を返す
    virtual DataSourceInfo info() const = 0;

    // データフレームを読み出す (ブロッキング or ノンブロッキング)
    virtual ReadResult read_frame(FrameBuffer& out) = 0;

    // データソースの開始/停止
    virtual void start() = 0;
    virtual void stop() = 0;
};
```

**実装済みの IDataSource:**
- **SyntheticSource** (`src/synthetic_source.h`): libgrebe 同梱、合成波形生成
- **IpcSource** (`apps/viewer/ipc_source.h`): grebe-viewer 同梱、grebe-sg からのパイプ受信

**設計判断: コピーベースを v1.0 のデフォルトとする根拠**

PCIe DMA や AF_XDP のゼロコピーパスに対応するにはバッファ借用セマンティクス（`borrow_frame()` / `release_frame()`）が必要だが、v1.0 ではコピーベースの `read_frame()` のみとする。TR-001 §3.2 の知見により、パフォーマンスのボトルネックは memcpy のコスト自体ではなくキャッシュコールドデータアクセスであり、マルチスレッドデシメーションによる局所性改善の方が支配的に効くことが判明しているためである。

| フェーズ | IDataSource API | 対応デバイス |
|---------|-----------------|-------------|
| v1.0 | `read_frame()` — コピーベース | 合成信号, ファイル, USB (libusb) |
| v1.1 (将来) | `borrow_frame()` / `release_frame()` 追加 | PCIe DMA (VFIO), AF_XDP |
| v2.0 (将来) | リングバッファ共有登録 | デバイスが libgrebe のリングに直接 DMA |

#### IRenderBackend

描画バックエンドの抽象インタフェイス。libgrebe はヘッダのみ（`include/grebe/render_backend.h`）を提供し、具体実装はアプリケーション側に配置する。grebe-viewer が VulkanRenderer 実装を同梱する。

```cpp
class IRenderBackend {
public:
    virtual ~IRenderBackend() = default;

    // 頂点データの GPU アップロード
    virtual void upload_vertices(const int16_t* data, size_t count) = 0;

    // 完了した転送を描画スロットに昇格
    virtual bool swap_buffers() = 0;

    // マルチチャネル波形フレームの描画 (DrawRegion で描画領域を指定)
    virtual bool draw_frame(const DrawCommand* channels, uint32_t num_channels,
                            const DrawRegion* region) = 0;

    // ウィンドウリサイズ対応
    virtual void on_resize(uint32_t width, uint32_t height) = 0;

    // V-Sync 制御
    virtual void set_vsync(bool enabled) = 0;
    virtual bool vsync() const = 0;

    // 描画バッファ内の現在の頂点数
    virtual uint32_t vertex_count() const = 0;
};
```

**実装済み:** VulkanRenderer (`apps/viewer/vulkan_renderer.h`) — Vulkan ベースの高速描画、トリプルバッファリング、overlay callback 対応

### 3.3 フレームプロトコル

PoC で実装・検証済みの FrameHeaderV2。IPC パイプ通信で使用される。FrameHeaderV2 は libgrebe 外（`apps/common/ipc/contracts.h`）に配置され、grebe-viewer と grebe-sg の間で共有される。

```cpp
struct FrameHeaderV2 {
    uint32_t magic;                // 'GFH2' (0x32484647 LE)
    uint32_t header_bytes;         // sizeof(FrameHeaderV2)（前方互換用）
    uint64_t sequence;             // 単調増加、ドロップ検出用
    uint64_t producer_ts_ns;       // E2E レイテンシ計測用タイムスタンプ
    uint32_t channel_count;        // 1–8
    uint32_t block_length_samples; // チャネルあたりのサンプル数
    uint32_t payload_bytes;        // channel_count × block_length_samples × 2
    uint32_t header_crc32c;        // ヘッダ整合性検証 (プレースホルダ)
    double   sample_rate_hz;       // サンプルレート（動的変更対応）
    uint64_t sg_drops_total;       // ソース (grebe-sg) 側ドロップ累計
    uint64_t first_sample_index;   // 絶対サンプル位置
};
```

libgrebe 内部で使用するフレーム形式は `grebe::FrameBuffer`（`include/grebe/data_source.h`）であり、IPC ヘッダからの変換は IpcSource（アプリケーション側）が行う。

### 3.4 検証済み設計制約 (TR-001 由来)

以下は PoC で定量的に検証済みであり、本ライブラリでも維持する設計制約である。

| 制約 | 根拠 | TR-001 参照 |
|------|------|------------|
| デシメーションは GPU ではなく CPU 側で実行する | CPU SIMD 19,834 MSPS vs GPU Compute 5,127 MSPS (3.9×) | §3.1 |
| デシメーション後の GPU 転送量は ≤ 7.68 KB/frame | 1920×2 = 3,840 頂点 × 2 bytes; 入力レート非依存 | §1.2 |
| マルチチャネルデシメーションはチャネル単位で並列化する | キャッシュ局所性: 4ch×1G 逐次 = 18.19ms, 並列 = 0.22ms (80×) | §3.2 |
| MSVC ビルドでは明示的 SIMD intrinsics が必須 | GCC 1.1× vs MSVC 10.5× (auto-vectorization 差) | §2.1 |
| ロックフリー SPSC リングバッファでスレッド間結合 | atomic のみ、mutex なし、<0.3% 充填率 @ 1 GSPS | §2.3 |
| GPU アップロードはトリプルバッファリングで重畳 | 書込/転送/描画を独立スロットで並行 | §2.5 |

---

## 4. 機能要件

### FR-01: DataSource 抽象インタフェイス

- IDataSource を実装することで任意のデータ取得インタフェイスを接続できること
- ライブラリは以下の参照実装を同梱する：
  - **SyntheticSource** (実装済み): 合成信号生成（正弦波、矩形波、鋸歯状波、ホワイトノイズ、チャープ）
- アプリケーション同梱の IDataSource 実装：
  - **IpcSource** (実装済み): grebe-sg からの IPC パイプ受信（grebe-viewer 同梱、`apps/viewer/`）
- 将来の参照実装（未実装）：
  - **FileSource**: バイナリファイルからの再生
  - **UdpSource**: UDP ソケット経由のデータ受信（grebe-udp-sender デモプログラムと対）
- 外部実装（高帯域 NIC (DPDK/AF_XDP)、USB3、PCIe）はアプリケーション側で IDataSource を実装して注入する

### FR-02: デシメーションエンジン

- MinMax (SSE2 SIMD) および LTTB アルゴリズムをサポートすること
- サンプルレート ≥ 100 MSPS で LTTB を自動的に MinMax に切り替えること（PoC 実績: LTTB 734 MSPS < 1 GSPS）
- デシメーション出力は画面解像度に応じた頂点数 (width × 2) であること
- アルゴリズムの追加が可能な拡張ポイントを設けること

### FR-03: マルチチャネルサポート

- 1–8 チャネルの同時描画をサポートすること
- チャネルごとに独立したリングバッファとデシメーションワーカーを割り当てること
- チャネルの配色、表示レイアウト（垂直分割）はコンフィグで指定可能であること

### FR-04: レンダリングバックエンド

- IRenderBackend を実装することで描画先を差し替え可能であること
- libgrebe はインタフェイス定義（`include/grebe/render_backend.h`）のみを提供し、具体実装はアプリケーション側に配置する
- grebe-viewer は以下の実装を同梱する：
  - **VulkanRenderer** (実装済み): Vulkan ベースの高速描画（PoC で検証済み、`apps/viewer/`）
  - overlay callback (`std::function<void(VkCommandBuffer)>`) による描画拡張ポイント（HUD 等）
- プッシュ定数による per-channel 描画パラメータ（振幅スケール、オフセット、色、DrawRegion）を維持すること

### FR-05: テレメトリ / メトリクス API

- 以下のメトリクスをフレーム単位で取得可能な構造体 (`TelemetrySnapshot`) として公開すること：
  - FPS、フレーム時間（ローリング平均）
  - E2E レイテンシ
  - リングバッファ充填率
  - パイプライン各相タイミング（drain, upload, swap, render, decimation）
  - データレート、頂点数、デシメーション比
- libgrebe は `TelemetrySnapshot` データ型定義のみ提供（`include/grebe/telemetry.h`）
- テレメトリ計測ロジック（Benchmark クラス）、CSV/JSON シリアライズはアプリケーション側（grebe-viewer）に配置

### FR-06: 波形忠実度検証

- エンベロープ検証器（PoC 実装ベース）をプロファイリング機能として提供すること
- 既知の周期信号に対し ±1 LSB の許容誤差でエンベロープ一致率を算出すること
- 実装は grebe-viewer のプロファイラに同梱（`apps/viewer/envelope_verifier.h`）— libgrebe のコア機能ではない

### FR-07: コンフィギュレーション API

- 全設定を構造体ベースで受け渡しすること（CLI パーサーはライブラリに含めない）
- `PipelineConfig` (`include/grebe/config.h`) の設定項目：
  - チャネル数 (1–8)、リングバッファサイズ
  - デシメーション設定（`DecimationConfig`: アルゴリズム、ターゲット頂点数、可視時間幅）
  - V-Sync モード
- 将来の拡張項目（未実装）：
  - バックプレッシャーポリシー（ドロップ / ブロック / コールバック通知）

---

## 5. 非機能要件

### NFR-01: 描画性能

PoC 実績（TR-001 §概要）を基準とした性能目標。環境: x86_64, Vulkan 対応 GPU。

| レベル | 入力条件 | 目標 FPS | PoC 実績 |
|--------|----------|----------|----------|
| L0 | 1 ch × 1 MSPS, MinMax | ≥ 60 FPS | 60.3 FPS |
| L1 | 1 ch × 100 MSPS, MinMax | ≥ 60 FPS | 60.2 FPS |
| L2 | 1 ch × 1 GSPS, MinMax | ≥ 60 FPS | 59.7 FPS |
| L3 | 1 ch × 1 GSPS, MinMax, V-Sync OFF | ≥ 500 FPS | 2,022 FPS |

### NFR-02: E2E レイテンシ

データフレーム到着からピクセル表示完了までの遅延。

| レベル | 条件 | p99 目標 | PoC 実績 |
|--------|------|----------|----------|
| L1 | 1 ch, 任意レート | ≤ 50 ms | 18.1 ms |
| L2 | 4 ch, 任意レート | ≤ 100 ms | 6.6 ms |

### NFR-03: 波形忠実度

- デシメーションアルゴリズムの出力が、入力信号のエンベロープに対し ±1 LSB 以内であること
- 定常状態でのエンベロープ一致率 ≥ 99%（PoC 実績: Embedded 100%, IPC 4ch×1G 99.2%）

### NFR-04: ゼロドロップ（定常状態）

- DataSource がデータを供給し続ける限り、ライブラリ内部でのサンプルドロップが 0 であること
- ドロップが発生した場合はテレメトリ API を通じて検出可能であること

### NFR-05: リソース効率

- ライブラリ初期化後にヒープアロケーションを行わないこと（リングバッファ、ワーカープール等は初期化時に確保）
- CPU 使用率: デシメーション + 描画で利用可能コア数の 50% 以下（8ch × 1 GSPS 時）

### NFR-06: クロスプラットフォーム

- Linux (GCC) と Windows (MSVC) で同一ソースからビルド可能であること
- 性能差は同一ハードウェアで 20% 以内であること（SIMD intrinsics の明示使用で保証）

---

## 6. 成果物

### 6.1 ライブラリ

| 成果物 | 状態 | 説明 |
|--------|------|------|
| libgrebe | 実装済み | コアデータパイプラインライブラリ（静的リンク）、spdlog のみ依存 |
| `include/grebe/grebe.h` | 実装済み | パブリック API アンブレラヘッダ |
| CMake パッケージ | 未実装 | `find_package(grebe)` で利用可能な CMake config |

### 6.2 アプリケーション

| 成果物 | 状態 | 説明 |
|--------|------|------|
| grebe-viewer | 実装済み | 参照実装ビューア（Vulkan + ImGui + IPC + プロファイラ + ベンチマーク） |
| grebe-sg | 実装済み | 信号発生器プロセス（OpenGL + ImGui GUI、IPC パイプ出力、grebe-viewer から自動起動） |
| grebe-bench | スタブ | パフォーマンスベンチマークスイート（libgrebe API ベース移行予定） |
| grebe-udp-sender | 未実装 | UdpSource 検証用の UDP 送信デモ（将来） |

### 6.3 ドキュメント

| 成果物 | 状態 | 説明 |
|--------|------|------|
| doc/RDD.md | 実装済み | 本要件定義書 |
| doc/TR-001.md | 実装済み | PoC 技術レポート（検証済みエビデンス） |
| doc/TI-phase7.md | 実装済み | Phase 7 リファクタリング性能回帰検証レポート |
| doc/API.md | 未実装 | パブリック API リファレンス |
| doc/INTEGRATION.md | 未実装 | DataSource / RenderBackend 統合ガイド |

---

## 7. リスクと制約

| リスク | 影響 | 緩和策 |
|--------|------|--------|
| 実デバイスのデータ特性（バースト、ジッタ、欠損）が合成データと異なる | デシメーション品質の低下、予期せぬドロップ | バックプレッシャーポリシーの柔軟化、ジッタ許容バッファの追加 |
| PCIe DMA はゼロコピーが必要だがリングバッファは memcpy 前提 | 高帯域デバイスでの性能低下 | IDataSource にゼロコピーパスを拡張可能な設計とする |
| Vulkan 以外の GPU API (Metal, DirectX) の需要 | ユーザ層の制限 | IRenderBackend 抽象で将来対応可能な構造を維持 |
| マルチプロセスでの利用（計測器 + 表示が別プロセス） | IPC レイヤが必要 | IPC パイプ実装を grebe-viewer/grebe-sg で検証済み（`apps/common/ipc/`）、他のトランスポートもアプリ側で追加可能 |
| AVX2/AVX-512 非対応 CPU での性能低下 | SSE2 フォールバックは十分高速 (19,834 MSPS) だが最適ではない | コンパイル時ディスパッチで最適命令セットを自動選択 |

---

## 付録 A: PoC からの移行マッピング

PoC (v0.1.0) の主要コンポーネントと現在のモジュール配置の対応。

| PoC コンポーネント | PoC ファイル | 現在の配置 | 変更内容 |
|-------------------|-------------|-----------|----------|
| DataGenerator | data_generator.h/cpp | `src/` (libgrebe) | SyntheticSource の内部エンジンとして残留 |
| SyntheticSource | *(Phase 2 で新設)* | `src/synthetic_source.h/cpp` (libgrebe) | IDataSource 実装、DataGenerator をラップ |
| RingBuffer | ring_buffer.h | `src/ring_buffer.h` (libgrebe) | ロックフリー SPSC キュー |
| Decimator | decimator.h/cpp | `src/decimator.h/cpp` (libgrebe) | MinMax SIMD + LTTB |
| DecimationThread | decimation_thread.h/cpp | `src/decimation_thread.h/cpp` (libgrebe 内部) | DecimationEngine ファサード経由で使用 |
| DecimationEngine | *(Phase 4 で新設)* | `include/grebe/decimation_engine.h` (公開 API) | 公開ファサード |
| IngestionThread | *(Phase 2 で新設)* | `src/ingestion_thread.h/cpp` (libgrebe) | DataSource → RingBuffer ドライバ |
| BufferManager | buffer_manager.h/cpp | `apps/viewer/` (grebe-viewer) | VulkanRenderer 内部 |
| Renderer | renderer.h/cpp | `apps/viewer/renderer.h/cpp` (grebe-viewer) | overlay callback 対応、libgrebe から分離 |
| VulkanRenderer | *(Phase 3 で新設)* | `apps/viewer/vulkan_renderer.h/cpp` (grebe-viewer) | IRenderBackend 実装 |
| Benchmark | benchmark.h/cpp | `apps/viewer/benchmark.h/cpp` (grebe-viewer) | テレメトリ計測、CSV ロギング |
| Microbench | microbench.h/cpp | `apps/viewer/microbench.h/cpp` (grebe-viewer) | 独立マイクロベンチマーク (BM-A/B/C/E/F) |
| Profiler | profiler.h/cpp | `apps/viewer/profiler.h/cpp` (grebe-viewer) | 自動プロファイリング + JSON レポート |
| HUD | hud.h/cpp | `apps/viewer/hud.h/cpp` (grebe-viewer) | ImGui テレメトリ表示 |
| PipeTransport | ipc/pipe_transport.h/cpp | `apps/common/ipc/` (grebe-viewer/sg 共有) | IPC パイプ実装 |
| IpcSource | *(Phase 2 で新設)* | `apps/viewer/ipc_source.h/cpp` (grebe-viewer) | IDataSource 実装 (IPC パイプ) |
| EnvelopeVerifier | envelope_verifier.h/cpp | `apps/viewer/envelope_verifier.h/cpp` (grebe-viewer) | プロファイラ内の検証ロジック |
| ProcessHandle | process_handle.h/cpp | `apps/viewer/process_handle.h/cpp` (grebe-viewer) | grebe-sg サブプロセス起動 |

---

## 付録 B: データ量計算

| 構成 | 入力レート | 1 フレーム (60 FPS) | デシメーション後 |
|------|-----------|--------------------|--------------------|
| 1ch × 1 MSPS | 2 MB/s | 33 KB | 7.68 KB |
| 1ch × 100 MSPS | 200 MB/s | 3.3 MB | 7.68 KB |
| 1ch × 1 GSPS | 2 GB/s | 33 MB | 7.68 KB |
| 4ch × 1 GSPS | 8 GB/s | 133 MB | 30.72 KB |
| 8ch × 1 GSPS | 16 GB/s | 267 MB | 61.44 KB |

デシメーション後の GPU 転送量は入力レートに依存せず、画面解像度（1920px 想定）とチャネル数のみに比例する。

---

## 付録 C: IDataSource 参照実装パターン

各デバイス種別における IDataSource 実装の想定構成を示す。現時点で実装済みなのは SyntheticSource（libgrebe 同梱）と IpcSource（grebe-viewer 同梱）のみ。FileSource、UdpSource は将来実装予定。他はアプリケーション側の実装例として位置づける。

### C.1 SyntheticSource（libgrebe 同梱、実装済み）

PoC の DataGenerator に相当。テスト・デモ・ベンチマーク用。

```
SyntheticSource
  │ 周期タイリング (memcpy) or LUT + 位相累積
  │ → FrameHeader 生成
  │ → read_frame() で FrameBuffer にコピー
  ▼
libgrebe RingBuffer
```

- バッファ所有権: ユーザ (libgrebe) が確保
- 通知: 同期（read_frame がデータ生成してすぐ返る）
- 帯域制約: CPU 速度依存（PoC 実績: ≥ 1 GSPS @ period tiling）

### C.2 FileSource（将来実装予定）

キャプチャ済みバイナリファイルの再生。

```
FileSource
  │ mmap(file) + madvise(MADV_SEQUENTIAL)
  │ → mmap 領域から直接 FrameBuffer にコピー
  │ → サンプルレートに応じたペーシング
  ▼
libgrebe RingBuffer
```

- バッファ所有権: カーネルページキャッシュ → FrameBuffer にコピー
- 通知: 同期（mmap アクセスはページフォルトで暗黙ブロック）
- 帯域制約: NVMe SSD で 5–7 GB/s（1 GSPS = 2 GB/s に対し十分）

### C.3 UdpSource（将来実装予定）

UDP ソケット経由で ADC サンプルを受信する参照実装。OS 標準のソケット API のみを使用し、外部ライブラリ依存なしでネットワーク入力パスを検証できる。

```
grebe-udp-sender (デモプログラム)              UdpSource (libgrebe 同梱)
  │ SyntheticSource 相当の波形生成               │ recvmmsg() / WSARecvFrom()
  │ → FrameHeader + int16_t[] を                │ → UDP ペイロードから
  │   UDP パケットとして送信                      │   FrameHeader + サンプルを復元
  │ → ローカルループバック (127.0.0.1)            │ → FrameBuffer にコピー
  │   or 別ホストからの送信                       │
  └──────── UDP ──────────────────────────────────▶ libgrebe RingBuffer
```

- バッファ所有権: カーネルソケットバッファ → FrameBuffer にコピー
- 通知: ブロッキング recv / epoll / IOCP（プラットフォーム依存）
- 帯域制約: ループバック ≈ 数 GB/s、1 GbE ≈ 125 MB/s（≈ 62.5 MSPS × 1ch）
- 外部依存: なし（POSIX ソケット / Winsock のみ）
- クロスプラットフォーム: Linux / Windows 双方で動作すること
- 検証戦略: grebe-udp-sender をローカルループバックで実行し、実デバイスなしで E2E パイプラインを検証

> **高帯域 NIC (DPDK / AF_XDP)**: 10 GbE 以上の帯域が必要な場合は、アプリケーション側で AF_XDP (libbpf) or DPDK (librte_*) を用いた IDataSource を実装して注入する。UdpSource のプロトコル処理部分を参考にできる。

### C.4 PCIe FPGA/ADC カード（アプリケーション実装例）

FPGA が PCIe DMA でホストメモリに直接サンプルデータを書き込む構成。

```
PcieDmaSource
  │ VFIO で PCIe BAR をユーザ空間にマップ
  │ → ベンダ DMA ドライバ (XDMA 等) で DMA 転送開始
  │ → eventfd or polling で DMA 完了検知
  │ → DMA バッファから FrameBuffer にコピー (v1.0)
  │   or BorrowedFrame で DMA バッファを直接参照 (v1.1)
  ▼
libgrebe RingBuffer
```

- バッファ所有権: ユーザが確保し IOMMU にマッピング
- 通知: eventfd（割り込み）or polling
- 帯域制約: PCIe Gen3 x16 ≈ 16 GB/s、Gen4 ≈ 32 GB/s
- 外部依存: VFIO + ベンダ固有ドライバ/API (Xilinx XDMA, Intel OPAE 等)
- フレーミング: デバイスファームウェア定義（IDataSource 実装側で FrameHeader に変換）

### C.5 USB 計測器（アプリケーション実装例）

USB 3.x バルク転送で接続する汎用計測器。

```
UsbSource
  │ libusb_submit_transfer() で非同期バルク転送をキューイング
  │ → コールバックでデータ受信
  │ → USB パケットを結合・FrameBuffer にコピー
  ▼
libgrebe RingBuffer
```

- バッファ所有権: ユーザが確保し libusb に渡す
- 通知: コールバック (async) or ブロッキング (sync)
- 帯域制約: USB 3.0 ≈ 実効 3 Gbps、USB 3.2 Gen2 ≈ 10 Gbps
- 外部依存: libusb 1.0
- 制約: VFIO によるカーネルバイパス不可（USB プロトコルスタックが必須）
