# Grebe — Integration Guide

**Version:** 2.1.0
**Last Updated:** 2026-02-11

このドキュメントは、`doc/RDD.md` (v3.1.0) の要件を実装に落とし込むための統合ガイドである。
RDD が「契約レベル」を扱うのに対し、本書は「実装パターン」を扱う。

ステータス語彙:

- `Current`: 現在実装で成立している状態
- `Target`: 本バージョンで達成を目指す状態
- `Planned`: 将来フェーズで実装予定

---

## 1. Scope

対象:

- Stage 実装の統合パターン
- SharedMemory データプレーンの統合
- Embedded/Pipe/UDP を SourceStage ingress として SharedMemory へ接続する統合
- 共有メモリ Queue の設計指針
- Frame 所有権モデル（Owned/Borrowed）の使い分け
- Fan-out 構成パターン
- OS/デバイス別 I/O メカニズムの選定

非対象:

- 製品要件・受入基準の定義（RDD を参照）

---

## 2. Core Contracts

### 2.1 Stage 契約

`IStage::process()` は BatchView を受け取り、BatchWriter に結果を書き出す。

最小契約:

- EOS / Retry / Error を戻り値で明示する
- stop 要求に対して bounded time で停止する
- Borrowed Frame を受け取った場合、処理完了後に release するか Owned に変換する

`IStage` が一次契約であり、`IDataSource` は legacy SourceStage アダプタとして扱う。
`SyntheticSource`, `TransportSource` は当面アダプタ経由で利用可能。
`IDataSource` アダプタは `v4.0` で廃止予定（`Planned`）。

`v4.0` 廃止ゲート（`Target`）:

- built-in Source が `IStage` ネイティブ実装へ移行完了
- デフォルト構成と CI テスト経路から `IDataSource` アダプタ依存を除去
- SharedMemory システム NFR（NFR-02〜NFR-08）・
  SourceStage 入力 NFR（NFR-09〜NFR-11）・
  VisualizationStage NFR（NFR-01, NFR-12）を `grebe-bench` で継続合格

### 2.2 Frame Protocol

プロセス間伝送（Pipe/UDP 境界）は `FrameHeaderV2` + payload を基本単位とする。

- payload: `[ch0][ch1]...[chN-1]` の channel-major
- `payload_bytes = channel_count * block_length_samples * sizeof(int16_t)`
- viewer 側で `TransportSource` が `FrameBuffer` に変換

### 2.3 Frame 所有権モデル

| モデル | 生成タイミング | 解放責務 |
|---|---|---|
| Owned | Pipe/UDP 受信時のコピー、SyntheticSource 生成 | Frame デストラクタ（通常の RAII） |
| Borrowed | SharedMemory 読み取り、DMA バッファ参照 | 明示的 release（プロデューサに返却） |

使い分けの指針:

- **Owned を使う場合**: プロセス間コピーが不可避な経路（Pipe, UDP）、
  または Stage が出力データを変異させる場合
- **Borrowed を使う場合**: 共有メモリ上のデータを読み取り専用で参照する経路。
  Stage 内でデータを変更する必要がある場合は Owned に変換してから処理する

---

## 3. Built-in Integration Patterns

### 3.1 SyntheticSource (libgrebe)

用途: テスト、デモ、ベンチマーク。
方式: period tiling または LUT で `FrameBuffer` を生成して返却。
所有権: Owned（生成時にバッファを確保）。

### 3.2 FileReader (grebe-sg)

用途: GRB ファイル再生。
方式: `mmap` ベースで読み取り、ペーシングしつつ sender thread へ供給。
所有権: Owned（mmap 領域から RingBuffer にコピー）。

### 3.3 Pipe Transport

用途: ローカル標準運用。
方式: `grebe-sg` を viewer が subprocess として起動し、stdout/stdin パイプで送受信。
所有権: Owned（`read` で受信バッファにコピー）。

### 3.4 UDP Transport

用途: 独立プロセス運用、ネットワーク越し運用。
方式: `UdpProducer`/`UdpConsumer` による datagram 伝送。
所有権: Owned（`recvfrom` で受信バッファにコピー）。

実装メモ:

- [Current] WSL2 loopback は datagram サイズ制約が厳しい
- [Current] Windows native では大きい datagram が有効
- [Current] scatter-gather I/O (sendmsg/WSASendTo) により送信側 memcpy を削減
- [Current] sendmmsg/recvmmsg バッチ I/O (Linux) で syscall overhead を低減

### 3.5 SharedMemory Transport [Target]

用途: 同一マシン高帯域、マルチコンシューマ。
方式: 共有メモリ領域上の **コンシューマ別独立 Queue** を介してプロセス間通信。
所有権: **Borrowed**（コンシューマは shm 上のデータを直接参照、release で返却）。

構成パターン:

```
Producer process:
  SourceStage → ShmWriter
    - 共有 payload pool にフレームデータを書き込み
    - 各コンシューマ専用 Queue に参照 descriptor を enqueue

Consumer process(es):
  ShmReader → ProcessingStage → ...
    - 自分専用 Queue の read index を atomic に追跡
    - Borrowed Frame として下流に渡す
    - 処理完了後に release（descriptor を返却）
```

設計指針:

- 共有メモリ領域のサイズは初期化時に固定する（定常状態で確保しない）
- データ整合性は atomic + memory fence で担保し、待機は event 通知または polling で行う
- コンシューマの crash を heartbeat またはタイムアウトで検知する
- fan-out はコンシューマごとの独立 Queue で構成する

---

## 4. Fan-Out Patterns

### 4.1 概要

一つの Stage 出力を複数の下流 Stage に同時配信するパターン。
RDD FR-11 の実装指針。

```
                    ┌─ [Queue A] → Decimation → Visualization
Source → [Queue] ──┤
                    ├─ [Queue B] → FFTStage → SpectrumView
                    │
                    └─ [Queue C] → Recorder
```

### 4.2 In-Process Fan-Out

同一プロセス内で Runtime が出力 Frame を各 Queue にコピー（Owned）またはクローン。

指針:

- 各 Queue に独立した backpressure policy を設定する
- 遅い消費者が速い消費者をブロックしない（独立性保証）
- 遅い消費者には `drop_oldest` policy を推奨

### 4.3 SharedMemory Fan-Out

共有 payload pool + コンシューマ別 Queue で複数プロセスに配信。

指針:

- プロデューサは payload を 1 回書き込み、各コンシューマ Queue に参照を配布
- Queue はコンシューマごとに独立し、遅いコンシューマが他系統をブロックしない
- payload 再利用は参照カウントを主方式とし、障害時は lease/timeout で強制回収する
- 遅延コンシューマはタイムアウトで切り離し可能にする

---

## 5. Device-Specific Source Patterns

### 5.1 High-Bandwidth NIC (Linux)

候補:

- AF_XDP
- DPDK

設計指針:

- NIC キューとスレッドを 1:1 に近づける
- 受信バーストをフレーム境界に再構成して SourceStage 出力とする
- backpressure 時は drop policy を明示する
- SharedMemory 経路: NIC → SourceStage → ShmWriter で他プロセスに配信

### 5.2 PCIe FPGA/ADC (Linux)

候補:

- VFIO + ベンダ DMA ドライバ（XDMA/OPAE 等）

設計指針:

- DMA バッファと IOMMU マッピングを初期化時に確定
- 完了通知は eventfd または polling
- Borrowed Frame で DMA バッファを直接参照可能
  （Stage 内で変更が必要な場合は Owned に変換）

### 5.3 USB Measurement Devices

候補:

- libusb async bulk

設計指針:

- transfer を複数 outstanding にしてジッタを吸収
- callback で受信したパケットをフレーム境界で結合
- ホスト側で再同期可能な sequence を持たせる

### 5.4 File Playback

候補:

- mmap + madvise(SEQUENTIAL)
- io_uring (要件次第)

設計指針:

- ファイル末尾のループ時に sequence 連続性の方針を固定
- 速度制御は source 側で行い viewer 側を過負荷にしない

---

## 6. OS Mechanism Matrix

| Mechanism | OS | Main Use | Zero-Copy | Kernel Bypass |
|---|---|---|---|---|
| VFIO / UIO | Linux | PCIe DMA | Yes | Yes |
| AF_XDP | Linux | NIC | Yes | Partial |
| DPDK | Linux | NIC | Yes | Yes |
| libusb async bulk | Linux/Windows/macOS | USB | Partial | No |
| io_uring | Linux | File/NIC | Partial-Yes | Partial |
| mmap | Cross-platform | File | Yes | N/A |
| IOCP | Windows | Socket/File | Partial | No |
| POSIX shm (shm_open) | Linux | SharedMemory Queue | Yes | N/A |
| Windows shared memory | Windows | SharedMemory Queue | Yes | N/A |

---

## 7. Buffer Ownership / Notification / Framing

| Axis | DPDK (NIC) | libusb (USB) | VFIO DMA | mmap (File) | SharedMemory |
|---|---|---|---|---|---|
| Buffer ownership | mempool | user-managed | user + IOMMU | page cache | shm region (固定長) |
| Notification | polling | callback/blocking | eventfd/polling | synchronous/fault | atomic + event/polling |
| Framing unit | packet | transfer | device-defined | byte stream | Frame 契約準拠 |
| Frame 所有権 | Owned (コピー後) | Owned | Borrowed | Owned (コピー後) | Borrowed |

---

## 8. Fault Isolation Patterns

### 8.1 コンシューマ crash

- プロデューサは各コンシューマの heartbeat（最終 read timestamp）を監視する
- タイムアウト（NFR-06: 1 秒）を超過したコンシューマの Queue を切り離す
- 切り離し後、プロデューサは該当領域を再利用可能にする
- 残りのコンシューマは影響を受けない

### 8.2 プロデューサ crash

- コンシューマは write index の停止 + heartbeat 不在を検知する
- EOS として扱い、graceful に停止する
- 共有メモリ領域のクリーンアップはプロセス終了時に OS が回収、
  または明示的な unlink で解放

### 8.3 共有メモリの一貫性

- ヘッダ（sequence, channel_count 等）は atomic 書き込みまたは seqlock で保護
- payload は write index 更新前に書き込み完了を保証（memory fence）
- コンシューマは read 時に sequence の整合性を検証する

---

## 9. Practical Checklist

### 9.1 新しい Stage を追加する場合

1. `IStage::process()` を実装（BatchView → BatchWriter）
2. Borrowed Frame を受け取る可能性がある場合、release パスを実装
3. stop 要求で bounded time に停止することを確認
4. Stage 単位の telemetry（throughput, latency, drops）を公開
5. `grebe-bench` に性能測定シナリオを追加

### 9.2 新しい SourceStage（デバイス統合）を追加する場合

1. 直接 `IStage` を実装（一次契約）
2. 移行期間のみ `IDataSource` adapter を利用可能（`v3.x`, `v4.0` 廃止予定）
3. `DataSourceInfo` の定義（ch/rate/realtime）
4. フレーミング規約（channel-major, payload bytes）
5. drop 計測（source 側 + viewer 側）
6. 停止シーケンス（`stop()` で確実に unblock）
7. 所有権モデルの決定（Owned: コピー経路 / Borrowed: ゼロコピー経路）
8. 新規実装では `IDataSource` adapter 依存を増やさない（`IStage` ネイティブを優先）

### 9.3 SharedMemory 経路を追加する場合

1. 共有メモリ領域のサイズを算出（チャネル数 × ブロックサイズ × スロット数）
2. プロデューサ/コンシューマの同期方式を選定（整合性: atomic + fence、待機: event/polling）
3. Borrowed Frame の release パスを全下流 Stage で実装
4. コンシューマ crash 時の切り離しタイムアウトを設定
5. Fan-out コンシューマ数の上限を決定（read index の管理コスト）
6. NFR-04 (ゼロコピー効率) の測定項目を `grebe-bench` に追加
