# Grebe — Stage/Interface 実行フレームワーク 要件定義書

**バージョン:** 3.0.0  
**最終更新:** 2026-02-11

> 本書は、grebe を「高速波形描画アプリ」ではなく、
> **高効率な Stage と Interface を接続・実行する E2E フレームワーク**として再定義する。
> 実装手順・OS 固有最適化・統合例は `doc/INTEGRATION.md` を参照。

---

## 1. 目的

高速時系列データ処理を以下の形で統一的に扱える基盤を定義する。

- Stage（処理単位）をカスケード接続してデータフローを構築
- Interface（接続契約）を共通化して実装差異を局所化
- Runtime が実行効率（スループット、レイテンシ、ドロップ率）を保証

### 1.1 スコープ

対象:

- E2E システム: `libgrebe` + `grebe-viewer` + `grebe-sg`
- Stage 契約、Frame 契約、実行 Runtime 契約
- モード別 NFR（Embedded / Pipe / UDP）

非対象:

- DPDK/AF_XDP/VFIO/IOCP 等の実装詳細（`doc/INTEGRATION.md` へ分離）

---

## 2. システム定義

### 2.1 成果物

| 成果物 | 役割 |
|---|---|
| `libgrebe` | Stage 実行 Runtime + コア処理 Stage 群 |
| `grebe-viewer` | 可視化 Stage 群 + UI/HUD + 計測 |
| `grebe-sg` | 生成/再生 Source Stage + 送信 Stage |

### 2.2 動作モード

| モード | 経路 | 主用途 |
|---|---|---|
| Embedded | Source(Stage, in-process) -> Runtime -> Visualization(Stage) | 上限性能・デバッグ |
| Pipe | grebe-sg -> pipe -> grebe-viewer | ローカル標準運用 |
| UDP | grebe-sg -> UDP -> grebe-viewer | 独立プロセス / ネットワーク運用 |

---

## 3. アーキテクチャ原則

1. **Stage First**
   - Source/Processing/Visualization を同じ Stage 抽象で扱う。
2. **Interface First**
   - Stage 間接続は共通 Frame 契約で統一し、実装依存を境界で止める。
3. **Runtime-Owned Efficiency**
   - 最適化（バッチ、キュー、スレッド配置、バックプレッシャー）は Runtime の責務とする。
4. **Data Plane / Control Plane 分離**
   - データ転送と設定変更・監視を分離し、性能劣化と複雑性を抑える。
5. **Mode-Invariant Contracts**
   - Embedded/Pipe/UDP で Stage 契約は不変。差分は Transport Stage 内に閉じ込める。

---

## 4. 論理アーキテクチャ

```
SourceStage -> Stage -> Stage -> ... -> VisualizationStage
      |            Runtime (queues, pool, scheduler)            |
      +--------------------- Control Plane ----------------------+
```

### 4.1 Stage 分類（論理）

| 分類 | 例 | 入力 | 出力 |
|---|---|---|---|
| SourceStage | Synthetic, File, Device, TransportRx | なし | Frame |
| TransformStage | 校正、再整列、レート変換 | Frame | Frame |
| ProcessingStage | Decimation, Feature extraction | Frame | Frame |
| PostStage | 可視化向け整形、レイアウト計算 | Frame | Frame |
| VisualizationStage | Render backend bridge | Frame | Draw commands / pixels |
| SinkStage | Recorder, Export, Metrics sink | Frame | なし |

### 4.2 Runtime 責務

- Stage グラフ（線形/DAG）の構築と実行
- Queue/Buffer Pool のライフサイクル管理
- Backpressure policy の適用
- スレッド/CPU アフィニティ管理
- テレメトリ収集（Stage 単位 + E2E）

---

## 5. 契約（Contract）

### 5.1 Frame 契約

Frame は以下を満たすこと。

- channel-major (`[ch0][ch1]...`) の `int16_t` データ
- 単調増加 `sequence`
- `producer_ts` を含む時刻情報
- 所有権モデル（owned/borrowed）を明示

参考メタデータ:

| 項目 | 意味 |
|---|---|
| `sequence` | 欠落検出・順序保証 |
| `channel_count` | チャネル数 |
| `samples_per_channel` | チャネルあたりサンプル数 |
| `sample_rate_hz` | レート |
| `flags` | borrowed/owned, discontinuity 等 |

### 5.2 Stage 契約

Stage は以下の共通契約を満たす。

- 入力 Batch を受け取り、出力 Batch を返す
- EOS / Retry / Error を明示的に返す
- stop 要求に対して bounded time で停止する

疑似シグネチャ:

```cpp
class IStage {
public:
    virtual ~IStage() = default;
    virtual StageResult process(const BatchView& in, BatchWriter& out, ExecContext& ctx) = 0;
};
```

### 5.3 Queue 契約

- 有界キュー（capacity 明示）
- 輻輳時の動作を policy 化
- policy は最低限 `drop_latest`, `drop_oldest`, `block` をサポート

### 5.4 Telemetry 契約

最低限、以下を Stage ごとと E2E で観測可能にする。

- throughput (frames/s, samples/s)
- latency (avg/p95/p99)
- drops (source/view/runtime)
- queue fill ratio
- processing time (stage breakdown)

---

## 6. 機能要件

### FR-01: Stage 抽象

- 全処理単位を `IStage` 互換で実装可能であること。
- Source/Sink/Visualization も同一抽象体系で扱えること。

### FR-02: Graph 構成

- Stage を線形または DAG として接続できること。
- 実行順序、fan-in/fan-out、バッチ境界を宣言可能であること。

### FR-03: Runtime 実行

- Runtime が Stage グラフの実行・同期・停止を管理すること。
- Stage 単位で worker 数と実行ポリシーを設定可能であること。

### FR-04: Data Source 統合

- `IDataSource` 準拠実装を SourceStage として注入可能であること。
- 既存 `SyntheticSource` と `TransportSource` を継続利用できること。

### FR-05: Processing 拡張

- Decimation は ProcessingStage として実装されること。
- 追加処理（FFT、トリガ、フィルタ等）を Stage 追加のみで統合できること。

### FR-06: Visualization 接続

- VisualizationStage は `IRenderBackend` 実装と接続可能であること。
- Render backend 差し替えで上流 Stage が変更不要であること。

### FR-07: Backpressure 制御

- queue ごとに policy を設定可能であること。
- policy 適用結果が telemetry に反映されること。

### FR-08: 互換トランスポート

- Pipe/UDP は Transport Stage として扱い、上流/下流契約を変更しないこと。
- 伝送ヘッダは `FrameHeaderV2` 互換を維持すること。

### FR-09: コンフィギュレーション

- 設定は構造体 API で受け渡すこと（CLI はアプリ層責務）。
- Runtime/Stage/Transport の設定境界を分離すること。

---

## 7. 非機能要件（モード別）

### NFR-01: 描画性能

| モード | 条件 | 目標 |
|---|---|---|
| Embedded | 1ch x 1 GSPS, MinMax, V-Sync ON | >= 60 FPS |
| Embedded | 1ch x 1 GSPS, MinMax, V-Sync OFF | >= 500 FPS |
| Pipe | 1ch x 100 MSPS | >= 60 FPS |
| Pipe | 1ch x 1 GSPS | >= 60 FPS |
| UDP | 1ch x 10 MSPS (loopback) | >= 60 FPS |
| UDP | 1ch x 100 MSPS (loopback) | >= 60 FPS |

### NFR-02: E2E レイテンシ (p99)

| モード | 条件 | 目標 |
|---|---|---|
| Embedded | 1ch, 任意レート | <= 50 ms |
| Pipe | 1ch/4ch, 任意レート | <= 100 ms |
| UDP | 1ch, 10-100 MSPS (loopback) | <= 100 ms |

### NFR-03: ゼロドロップ（定常状態）

| モード | 条件 | 目標 |
|---|---|---|
| Embedded | 1ch/4ch | 0 drops |
| Pipe | 1ch/4ch | 0 drops |
| UDP | 1ch x 10 MSPS | 0 drops |
| UDP | 1ch x 100 MSPS | 0 drops（環境依存条件付き） |

### NFR-04: スループット効率

- 高レート経路（>= 100 MSPS）ではフレームごとの再確保を避けること。
- Runtime は batch 実行をサポートし、1-frame 1-syscall 依存を緩和可能であること。

### NFR-05: リソース効率

- 固定バッファプールと有界キューを初期化時に確保すること。
- デシメーション+描画の CPU 使用率は 8ch x 1 GSPS 条件で利用可能コアの 50% 以下を目標とする。

### NFR-06: クロスプラットフォーム

- Linux (GCC) / Windows (MSVC) で同一ソースをビルド可能であること。
- Embedded/Pipe/UDP の全モードが両 OS で動作すること。

### NFR-07: 観測可能性

- Stage 単位と E2E の telemetry を同時収集できること。
- 問題解析時に「どの Stage が律速か」を判別できること。

---

## 8. 成果物

### 8.1 実装

| 成果物 | 状態 | 説明 |
|---|---|---|
| `libgrebe` | 実装済み（進化中） | Stage 実行基盤の中核 |
| `grebe-viewer` | 実装済み | Visualization 側参照実装 |
| `grebe-sg` | 実装済み | Source/Transport 側参照実装 |
| `grebe-bench` | 未完 | モード別 NFR 検証基盤 |

### 8.2 ドキュメント

| 文書 | 役割 |
|---|---|
| `doc/RDD.md` | 契約レベル要件 |
| `doc/INTEGRATION.md` | 実装統合ガイド |
| `doc/TR-001.md` | PoC 検証エビデンス |
| `doc/TI-phase9.1.md` | UDP 実測エビデンス |

---

## 9. リスクと制約

| リスク | 影響 | 緩和策 |
|---|---|---|
| Stage 数増加によるオーバーヘッド | レイテンシ増 | バッチ化、融合（stage fusion）戦略 |
| ownership 混在（owned/borrowed） | バグ、コピー増 | Frame 契約と lint/test を明文化 |
| UDP 帯域の環境依存 | モード差による誤判定 | NFR をモード別に分離し評価 |
| 実装詳細の再混入 | RDD の肥大化 | 詳細は INTEGRATION へ隔離 |

---

## 10. 今後の拡張方針

- v3.1: `borrow/release` ベースの zero-copy path を契約追加
- v3.2: DAG 実行最適化（fusion、NUMA-aware scheduling）
- v3.3: transport backend 抽象統一（sync/mmsg/iocp など）

