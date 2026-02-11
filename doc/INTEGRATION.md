# Grebe — Integration Guide

**Version:** 1.0.0  
**Last Updated:** 2026-02-11

このドキュメントは、`doc/RDD.md` の要件を実装に落とし込むための統合ガイドである。  
RDD が「契約レベル」を扱うのに対し、本書は「実装パターン」を扱う。

---

## 1. Scope

対象:

- DataSource 実装の統合
- Transport (Pipe/UDP) の統合
- OS/デバイス別 I/O メカニズムの選定
- バッファ所有権、通知モデル、フレーミングの設計指針

非対象:

- 製品要件・受入基準の定義（RDD を参照）

---

## 2. Core Contracts

### 2.1 IDataSource

`IDataSource` はデバイス固有 I/O を隠蔽し、libgrebe へ `FrameBuffer` を供給する。

最小契約:

- `info()` は `channel_count`, `sample_rate_hz`, `is_realtime` を返す
- `read_frame()` は channel-major の `int16_t` 配列を返す
- `start()/stop()` は再入可能かつ例外安全に実装する

実装時の注意:

- フレーム欠落時は `sequence` と drop counter で観測可能にする
- 高レート経路では固定長バッファを事前確保し、再確保を避ける

### 2.2 Frame Protocol

プロセス間伝送は `FrameHeaderV2` + payload を基本単位とする。

- payload: `[ch0][ch1]...[chN-1]` の channel-major
- `payload_bytes = channel_count * block_length_samples * sizeof(int16_t)`
- viewer 側で `TransportSource` が `FrameBuffer` に変換

---

## 3. Built-in Integration Patterns

### 3.1 SyntheticSource (libgrebe)

用途: テスト、デモ、ベンチマーク。  
方式: period tiling または LUT で `FrameBuffer` を生成して返却。

### 3.2 FileReader (grebe-sg)

用途: GRB ファイル再生。  
方式: `mmap` ベースで読み取り、ペーシングしつつ sender thread へ供給。

### 3.3 Pipe Transport

用途: ローカル標準運用。  
方式: `grebe-sg` を viewer が subprocess として起動し、stdout/stdin パイプで送受信。

### 3.4 UDP Transport

用途: 独立プロセス運用、ネットワーク越し運用。  
方式: `UdpProducer`/`UdpConsumer` による datagram 伝送。

実装メモ:

- WSL2 loopback は datagram サイズ制約が厳しい
- Windows native では大きい datagram が有効
- 高帯域化は `doc/TODO.md` Phase 9.2 を参照

---

## 4. Device-Specific Source Patterns

### 4.1 High-Bandwidth NIC (Linux)

候補:

- AF_XDP
- DPDK

設計指針:

- NIC キューとスレッドを 1:1 に近づける
- 受信バーストをフレーム境界に再構成して `FrameBuffer` 化
- backpressure 時は drop policy を明示する

### 4.2 PCIe FPGA/ADC (Linux)

候補:

- VFIO + ベンダ DMA ドライバ（XDMA/OPAE 等）

設計指針:

- DMA バッファと IOMMU マッピングを初期化時に確定
- 完了通知は eventfd または polling
- v1.0 はコピー経路、将来は borrow/release 経路を検討

### 4.3 USB Measurement Devices

候補:

- libusb async bulk

設計指針:

- transfer を複数 outstanding にしてジッタを吸収
- callback で受信したパケットをフレーム境界で結合
- ホスト側で再同期可能な sequence を持たせる

### 4.4 File Playback

候補:

- mmap + madvise(SEQUENTIAL)
- io_uring (要件次第)

設計指針:

- ファイル末尾のループ時に sequence 連続性の方針を固定
- 速度制御は source 側で行い viewer 側を過負荷にしない

---

## 5. OS Mechanism Matrix

| Mechanism | OS | Main Use | Zero-Copy | Kernel Bypass |
|---|---|---|---|---|
| VFIO / UIO | Linux | PCIe DMA | Yes | Yes |
| AF_XDP | Linux | NIC | Yes | Partial |
| DPDK | Linux | NIC | Yes | Yes |
| libusb async bulk | Linux/Windows/macOS | USB | Partial | No |
| io_uring | Linux | File/NIC | Partial-Yes | Partial |
| mmap | Cross-platform | File | Yes | N/A |
| IOCP | Windows | Socket/File | Partial | No |

---

## 6. Buffer Ownership / Notification / Framing

| Axis | DPDK (NIC) | libusb (USB) | VFIO DMA | mmap (File) |
|---|---|---|---|---|
| Buffer ownership | mempool | user-managed | user + IOMMU | page cache |
| Notification | polling | callback/blocking | eventfd/polling | synchronous/fault |
| Framing unit | packet | transfer | device-defined | byte stream |

---

## 7. Practical Checklist

新しい IDataSource を追加する際のチェック項目:

- 1. `DataSourceInfo` の定義（ch/rate/realtime）
- 2. フレーミング規約（channel-major, payload bytes）
- 3. drop 計測（source 側 + viewer 側）
- 4. 停止シーケンス（`stop()` で確実に unblock）
- 5. モード別 NFR に対する測定項目を `grebe-bench` に追加

