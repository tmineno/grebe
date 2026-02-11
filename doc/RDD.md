# Grebe — Stage/Interface 実行フレームワーク 要件定義書

**バージョン:** 3.1.0
**最終更新:** 2026-02-11

> 本書は、grebe を「高速波形描画アプリ」ではなく、
> **高効率な Stage と Interface を接続・実行する E2E フレームワーク**として再定義する。
> v0.2.0 (Phase 0–9.2) の PoC 成果を基盤に、共有メモリデータプレーンと
> マルチコンシューマ (fan-out) パイプラインをスコープに加える。
> 実装手順・OS 固有最適化・統合例は `doc/INTEGRATION.md` を参照。
>
> **Positioning:** 本書は **Target Architecture (v3.1)** を記述する。
> 実装現況との差分は `Current/Target/Planned` の語彙で明示する。

---

## 1. 目的

高速時系列データ処理を以下の形で統一的に扱える基盤を定義する。

- Stage（処理単位）をカスケードまたは DAG 接続してデータフローを構築
- Interface（接続契約）を共通化して実装差異を局所化
- Runtime が実行効率（スループット、レイテンシ、ドロップ率）を保証
- 共有メモリデータプレーンにより、デバイス入力から後段処理・可視化まで
  ゼロコピーで接続可能にする

### 1.1 スコープ

対象:

- E2E システム: `libgrebe` + `grebe-viewer` + `grebe-sg`
- Stage 契約、Frame 契約、実行 Runtime 契約
- NFR 階層（SharedMemory システム NFR + SourceStage 入力 NFR + VisualizationStage NFR）
- 共有メモリ上の Queue 契約と所有権モデル

非対象:

- DPDK/AF_XDP/VFIO/IOCP 等の実装詳細（`doc/INTEGRATION.md` へ分離）
- デバイスドライバ実装（アプリケーション側の責務）

### 1.2 ステータス語彙

本書の状態表記は以下に統一する。

| 語彙 | 意味 |
|---|---|
| `Current` | 現在の実装で成立している状態 |
| `Target` | 本バージョンで達成を目指す目標状態 |
| `Planned` | 将来フェーズで実装予定の状態 |

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
| Embedded | Source(Stage, in-process) -> ShmWriter -> shm region | SourceStage ingress 基準 |
| Pipe | grebe-sg -> pipe -> SourceStage -> ShmWriter -> shm region | ローカル ingress |
| UDP | grebe-sg -> UDP -> SourceStage -> ShmWriter -> shm region | 独立プロセス / ネットワーク ingress |
| SharedMemory | shm region -> Runtime -> Processing/Visualization/Sink | 同一マシン高帯域データプレーン |

本書の v3.1 Target では SharedMemory を主データプレーンとする。
Embedded/Pipe/UDP は SourceStage が SharedMemory に入力するための ingress バリエーションとして扱う。

SharedMemory モードの想定ユースケース:

- デバイスドライバ（NIC/PCIe/USB）が共有メモリにデータを配置し、
  grebe が読み取って可視化する
- 一つの Source から複数の後段処理（可視化・FFT・トリガ・録画等）が
  同一データを fan-out で受信する
- 同一マシン上の異なるプロセスが、ソケットオーバーヘッドなしに
  Stage 間通信を行う

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
   - Embedded/Pipe/UDP/SharedMemory で Stage 契約は不変。
   - 差分は Transport Stage および Queue backing 内に閉じ込める。
6. **Zero-Copy by Default**
   - 高帯域経路では Frame の所有権移転（move）または借用（borrow/release）を
     優先し、コピーを最小化する。コピーパスは低帯域の互換経路として維持する。
7. **Fan-Out Native**
   - Runtime は一つの出力を複数の下流 Stage に分配する fan-out を
     第一級の接続パターンとしてサポートする。

---

## 4. 論理アーキテクチャ

```
SourceStage -> [Queue] -> Stage -> [Queue] -> VisualizationStage
                                \
                                 +-> [Queue] -> ProcessingStage (FFT, Trigger, ...)
                                 +-> [Queue] -> SinkStage (Recorder, ...)
       |              Runtime (queues, pool, scheduler)              |
       +----------------------- Control Plane ------------------------+
```

### 4.1 Stage 分類（論理）

| 分類 | 例 | 入力 | 出力 |
|---|---|---|---|
| SourceStage | Synthetic, File, Device, TransportRx, ShmReader | なし | Frame |
| TransformStage | 校正、再整列、レート変換 | Frame | Frame |
| ProcessingStage | Decimation, FFT, Feature extraction | Frame | Frame |
| PostStage | 可視化向け整形、レイアウト計算 | Frame | Frame |
| VisualizationStage | Render backend bridge | Frame | Draw commands / pixels |
| SinkStage | Recorder, Export, Metrics sink, ShmWriter | Frame | なし |

### 4.2 Runtime 責務

- Stage グラフ（線形/DAG）の構築と実行
- Queue/Buffer Pool のライフサイクル管理
- Fan-out 接続（1 出力 → N 入力）の管理
- Backpressure policy の適用
- スレッド/CPU アフィニティ管理
- テレメトリ収集（Stage 単位 + E2E）

### 4.3 共有メモリデータプレーン

SharedMemory モードにおけるデータフロー:

```
 Process A (デバイスドライバ / grebe-sg)
 ┌─────────────────────────────────┐
 │ SourceStage                     │
 │   (NIC / PCIe / USB / Synth)   │
 │          │                      │
 │          ▼                      │
 │   ShmWriter (SinkStage)         │
 └──────────┬──────────────────────┘
            │ shared memory region
            │ (Queue 契約に準拠)
 ┌──────────▼──────────────────────┐
 │ Process B (grebe-viewer)        │
 │                                 │
 │ ShmReader ─┬─ Decimation ── Visualization
 │            ├─ FFTStage ──── SpectrumView
 │            └─ Recorder                  │
 └─────────────────────────────────────────┘
```

共有メモリ上の Queue は §5.3 Queue 契約に従う。
デバイスが直接共有メモリに書き込む場合、SourceStage は
borrow/release セマンティクスで Frame をゼロコピー参照する。

---

## 5. 契約（Contract）

### 5.1 Frame 契約

Frame は以下を満たすこと。

- channel-major (`[ch0][ch1]...`) の `int16_t` データ
- 単調増加 `sequence`
- `producer_ts` を含む時刻情報
- 所有権モデルを明示

**所有権モデル:**

| モデル | 意味 | 主な用途 |
|---|---|---|
| Owned | Frame がデータの排他的所有権を持つ | Pipe/UDP 受信後、低帯域経路 |
| Borrowed | 外部バッファ（shm, DMA）を参照。release で返却 | SharedMemory, PCIe DMA |

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
- Borrowed Frame を受け取った Stage は、処理完了後に release するか、
  Owned Frame に変換して下流に渡す

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

**Queue backing:**

| backing | プロセスモデル | ゼロコピー |
|---|---|---|
| In-process | 同一プロセス内 | move semantics |
| SharedMemory | 異なるプロセス間 | borrow/release |

SharedMemory backing の Queue は以下を追加で満たすこと:

- プロデューサ・コンシューマが異なるプロセスでも動作すること
- fan-out は **コンシューマごとの独立 Queue** で構成すること
- payload 再利用は参照カウントを主方式とし、障害時はタイムアウトで強制回収できること
- プロデューサまたはコンシューマのクラッシュが他方を無期限にブロックしないこと

### 5.4 Telemetry 契約

最低限、以下を Stage ごとと E2E で観測可能にする。

- throughput (frames/s, samples/s)
- latency (avg/p95/p99)
- drops (source/view/runtime)
- queue fill ratio
- processing time (stage breakdown)
- frame ownership transitions (borrow/release/copy 回数)

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

- Source の一次契約は `IStage` であること。
- 既存 `IDataSource` は移行用 SourceStage アダプタとして利用可能であること（`v3.x` 限定）。
- `SyntheticSource` と `TransportSource` はアダプタ経由で継続利用できること。
- `IDataSource` アダプタは `v4.0` で廃止予定（`Planned`）。

### FR-05: Processing 拡張

- Decimation は ProcessingStage として実装されること。
- 追加処理（FFT、トリガ、フィルタ等）を Stage 追加のみで統合できること。

### FR-06: Visualization 接続

- VisualizationStage は `IRenderBackend` 実装と接続可能であること。
- Render backend 差し替えで上流 Stage が変更不要であること。

### FR-07: Backpressure 制御

- Queue ごとに policy を設定可能であること。
- policy 適用結果が telemetry に反映されること。

### FR-08: 互換トランスポート

- Pipe/UDP は Transport Stage として扱い、上流/下流契約を変更しないこと。
- `FrameHeaderV2` 互換は **Pipe/UDP 境界** に限定して維持すること。
- Stage 内部契約は transport ヘッダに依存しないこと。

### FR-09: コンフィギュレーション

- 設定は構造体 API で受け渡すこと（CLI はアプリ層責務）。
- Runtime/Stage/Transport の設定境界を分離すること。

### FR-10: 共有メモリ Queue

- Queue の backing として共有メモリを選択可能であること。
- 共有メモリ Queue 経由で異なるプロセスの Stage が通信できること。
- プロデューサまたはコンシューマの異常終了後に、
  他方が検知してリカバリ可能であること。

### FR-11: Fan-Out

- 一つの SourceStage または ProcessingStage の出力を、
  複数の下流 Stage に同時配信できること。
- Fan-out 先ごとに独立した Queue と backpressure policy を持つこと。
- 遅い消費者が速い消費者をブロックしないこと（独立性）。

### FR-12: ゼロコピー Frame パス

- Borrowed Frame により、共有メモリ上のデータを
  コピーなしで下流 Stage に渡せること。
- Borrowed Frame を受け取った Stage が、
  必要に応じて Owned Frame に変換（コピー）できること。
- Owned Frame のみを扱う既存 Stage が変更なしで動作すること（後方互換）。

---

## 7. 非機能要件

本章の数値要件は **Target (v3.1)** を示す。

### NFR-00: NFR 階層管理

NFR は「SharedMemory システム要件」と「SourceStage 入力要件」に分離して管理する。
`grebe-bench` では下流の SharedMemory パイプライン条件を固定し、
ingress 差分（Embedded/Pipe/UDP）のみを比較可能にすること。

| レイヤ | 対象 | 主対象 NFR |
|---|---|---|
| SharedMemory システム | Runtime + Queue + 下流 Stage（Visualization を除く） | NFR-02〜NFR-08 |
| SourceStage ingress | Source 入力経路（Embedded/Pipe/UDP） | NFR-09〜NFR-11 |
| VisualizationStage | 可視化変換 + Render backend 提出 | NFR-01, NFR-12 |

SourceStage ingress NFR の評価対象は `SourceStage -> ShmWriter enqueue` 完了までとし、
下流 Processing/Visualization の性能は対象外とする。
VisualizationStage NFR は別プロファイルで評価すること。

### NFR-01: VisualizationStage 描画性能

| 条件 | 目標 |
|---|---|
| 1ch x 1 GSPS, MinMax, V-Sync ON | >= 60 FPS |
| 4ch x 1 GSPS, fan-out x2 (可視化 + 録画) | >= 60 FPS |

### NFR-02: SharedMemory E2E レイテンシ (p99)

| 条件 | 目標 |
|---|---|
| 1ch, 任意レート | <= 50 ms |
| 4ch, fan-out x2 | <= 100 ms |

### NFR-03: SharedMemory ゼロドロップ（定常状態）

| 条件 | 目標 |
|---|---|
| 1ch x 1 GSPS | 0 drops |
| 4ch x 1 GSPS, fan-out x2 | 0 drops |

### NFR-04: ゼロコピー効率

- SharedMemory 経路で SourceStage から ProcessingStage への
  frame data の memcpy が 0 回であること（borrow/release）。
- SourceStage 入力性能（NFR-09）を基準に、SharedMemory システム E2E レイテンシが
  20% 以内の増加に収まること。

### NFR-05: Fan-Out 独立性

- 遅い消費者の処理遅延が、他の消費者の throughput/latency に
  影響を与えないこと（独立 Queue による分離）。
- 消費者の追加・削除が、プロデューサの停止なしに行えること。

### NFR-06: 障害分離

- コンシューマプロセスのクラッシュ後、プロデューサが 1 秒以内に検知し、
  該当 Queue を切り離して継続動作すること。
- プロデューサのクラッシュ後、コンシューマが EOS を検知して
  graceful に停止できること。

### NFR-07: 観測可能性

- Stage 単位と E2E の telemetry を同時収集できること。
- 問題解析時に「どの Stage が律速か」を判別できること。
- SharedMemory Queue の充填率・borrow/release 頻度を観測可能であること。

### NFR-08: リソース効率

- SharedMemory Queue のメモリフットプリントは初期化時に固定されること。
- 固定バッファプールと有界キューで、定常状態でのヒープ確保を 0 にすること。

### NFR-09: SourceStage 入力性能（Embedded）

- in-process SourceStage が SharedMemory Queue へ連続投入できること。
- `SourceStage -> ShmWriter enqueue` latency (p99) が <= 10 ms であること。
- 定常状態でドロップ 0 を維持すること。

### NFR-10: SourceStage 入力性能（Pipe）

- Pipe 受信 SourceStage が SharedMemory Queue へ連続投入できること。
- `SourceStage -> ShmWriter enqueue` latency (p99) が同一条件 Embedded 比で +25% 以内であること。
- 定常状態でドロップ 0 を維持すること。

### NFR-11: SourceStage 入力性能（UDP）

- UDP 受信 SourceStage が SharedMemory Queue へ連続投入できること。
- `SourceStage -> ShmWriter enqueue` latency (p99) が同一条件 Embedded 比で +50% 以内であること。
- 定常状態でドロップ率 <= 0.1% であること。
- 計測結果は Windows native と WSL2 を分離して記録すること。

### NFR-12: VisualizationStage 処理レイテンシ

- `VisualizationStage::process` から Render backend 提出完了までの latency (p99) が <= 16 ms であること。
- 60 FPS 条件で frame miss 率 <= 0.1% を維持すること。
- 1ch と 4ch fan-out x2 条件で同一基準を満たすこと。

---

## 8. 成果物

### 8.1 実装

| 成果物 | 状態 | 説明 |
|---|---|---|
| `libgrebe` | `Current` | Stage 実行基盤の中核 |
| `grebe-viewer` | `Current` | Visualization 側参照実装 |
| `grebe-sg` | `Current` | Source/Transport 側参照実装 |
| `grebe-bench` | `Planned` | SharedMemory システム NFR + SourceStage 入力 NFR + VisualizationStage NFR 検証基盤 |

### 8.2 ドキュメント

| 文書 | 役割 |
|---|---|
| `doc/RDD.md` | 契約レベル要件（本書） |
| `doc/INTEGRATION.md` | 実装統合ガイド |
| `doc/TR-001.md` | PoC 検証エビデンス |
| `doc/TI-phase*.md` | フェーズ別実測エビデンス |

---

## 9. リスクと制約

| リスク | 影響 | 緩和策 |
|---|---|---|
| Stage 数増加によるオーバーヘッド | レイテンシ増 | バッチ化、融合（stage fusion）戦略 |
| ownership 混在（owned/borrowed） | バグ、ライフタイム違反 | Frame 契約の flags と telemetry で追跡 |
| 共有メモリのプロセス障害伝搬 | コンシューマ crash でプロデューサがハング | 有界タイムアウト + epoch ベースの無効化 |
| Fan-out 時の遅延コンシューマ | 最遅コンシューマが全体を支配 | コンシューマごとに独立 Queue + drop policy |
| NUMA 非対応配置 | キャッシュミス増、帯域低下 | 初期は単一ノード前提、将来 NUMA 対応 |
| 実装詳細の再混入 | RDD の肥大化 | 詳細は INTEGRATION.md へ隔離 |

---

## 10. 今後の拡張方針

| バージョン | 内容 |
|---|---|
| v3.2 | Stage/Runtime 基盤、SharedMemory Queue、fan-out 実装 |
| v3.3 | NUMA-aware scheduling、stage fusion |
| v3.4 | transport backend 抽象統一（sync/mmsg/iocp/shm） |
| v4.0 | IDataSource adapter 廃止、SharedMemory データプレーン + SourceStage ingress 安定、プロダクション品質 |

### 10.1 v4.0 `IDataSource` 廃止ゲート

- `SyntheticSource` / `TransportSource` を含む built-in Source が `IStage` ネイティブ実装へ移行完了していること。
- デフォルト構成および CI テスト経路に `IDataSource` adapter 依存が残っていないこと。
- SharedMemory システム NFR（NFR-02〜NFR-08）・SourceStage 入力 NFR（NFR-09〜NFR-11）・
  VisualizationStage NFR（NFR-01, NFR-12）が `grebe-bench` で継続合格していること。

---

## 付録 A: Legacy アーキテクチャからの主要変更

| 項目 | Legacy | v3.1 Target |
|---|---|---|
| アーキテクチャ | libgrebe データパイプライン | Stage/Interface 実行フレームワーク |
| 処理単位 | 固定パイプライン (Ingestion → Decimation → Render) | 汎用 Stage DAG |
| 接続モデル | SPSC リングバッファ (in-process) | Queue 契約 (in-process / shm) |
| データ所有権 | コピーベース (read_frame) | Owned / Borrowed 二層 |
| マルチコンシューマ | 非対応 | Fan-out (FR-11) |
| プロセス間通信 | Pipe / UDP | Pipe / UDP / SharedMemory |
| データソース抽象 | IDataSource | SourceStage (一次契約) + IDataSource adapter (v3.x 移行、v4.0 廃止予定) |
| 描画バックエンド抽象 | IRenderBackend | VisualizationStage (Stage 契約に統合) |

### 継続する設計制約（TR-001 由来）

以下は Legacy で定量検証済みであり、v3.1 Target でも維持する:

| 制約 | 根拠 |
|---|---|
| デシメーションは CPU SIMD で実行 | CPU 19,834 MSPS vs GPU 5,127 MSPS (3.9x) |
| デシメーション後 GPU 転送量 <= 7.68 KB/frame | 入力レート非依存 |
| ロックフリー SPSC で Stage 間結合 | atomic のみ、< 0.3% 充填率 @ 1 GSPS |
| MSVC では明示 SIMD intrinsics 必須 | GCC 1.1x vs MSVC 10.5x |

---

## 付録 B: データ量計算

| 構成 | 入力レート | 1 フレーム (60 FPS) | デシメーション後 |
|---|---|---|---|
| 1ch x 1 MSPS | 2 MB/s | 33 KB | 7.68 KB |
| 1ch x 100 MSPS | 200 MB/s | 3.3 MB | 7.68 KB |
| 1ch x 1 GSPS | 2 GB/s | 33 MB | 7.68 KB |
| 4ch x 1 GSPS | 8 GB/s | 133 MB | 30.72 KB |
| 8ch x 1 GSPS | 16 GB/s | 267 MB | 61.44 KB |

デシメーション後の GPU 転送量は入力レートに依存せず、
画面解像度（1920px 想定）とチャネル数のみに比例する。
この特性により、SharedMemory 経路で入力帯域が増加しても
描画側のボトルネックは変わらない。
