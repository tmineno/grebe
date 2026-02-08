# Grebe — 技術的検証項目 (Technical Judgment Log)

本文書はプロジェクトを通じて蓄積される技術的検証の履歴を管理する。各検証項目 (TI) に対し、計測・分析を行うたびにエントリを追記する。

---

## 変更履歴

| 日付 | Phase | 変更内容 |
|---|---|---|
| 2026-02-07 | Phase 3-4 | TI-01〜06 初回回答。llvmpipe (WSL2) 環境での計測結果に基づく |
| 2026-02-07 | Phase 6 | TI-01/03/05 実GPU回答追記。RTX 5080 (dzn) + Release ビルドでの計測。R-01完了 |
| 2026-02-07 | Phase 7 | TI-01/02/03/04 ネイティブ Vulkan 回答追記。MSVC ビルド + NVIDIA ネイティブドライバでの計測。R-10追加 |
| 2026-02-08 | Phase 9 | TI-07 IPC パイプ帯域と欠落率。anonymous pipe IPC vs embedded の性能比較。R-11追加 |
| 2026-02-08 | Phase 9 | TI-07 Windows ネイティブ追記。R-12 解消 (PeekNamedPipe)。Windows パイプ帯域 ~10-36 MB/s (WSL2 比 1/10〜1/30) |
| 2026-02-08 | Phase 10 | TI-07 Pipe 最適化後追記。バッファ 1MB + block 16K/64K + writev。Windows 帯域 ~10→~100-470 MB/s。Go/No-Go: Phase 11 延期 |
| 2026-02-08 | Phase 10-2 | TI-08 ボトルネック再評価。WSL2 Release 検証で drops 根本原因はパイプラインスループット（キャッシュ冷えデータ）と判明。shm 延期続行 |
| 2026-02-08 | Phase 10-3 | TI-08 追記。マルチスレッド間引き + リング 64M 拡大 + Debug 警告。4ch/8ch×1G 0-drops 達成。R-08/R-13 完了 |

---

## 検証項目一覧

| TI | 項目 | ステータス | 最終更新 |
|---|---|---|---|
| TI-01 | GPU 転送ボトルネック | 回答済み (llvmpipe + dzn + Native) | 2026-02-07 |
| TI-02 | CPU 間引き性能 | 回答済み (GCC + MSVC で比較) | 2026-02-07 |
| TI-03 | GPU 側間引きの可能性 | 回答済み (llvmpipe + dzn + Native) | 2026-02-07 |
| TI-04 | 描画プリミティブ選択 | 回答済み (Native で更新) | 2026-02-07 |
| TI-05 | 永続マップドバッファ | 検証不可 (現設計で恩恵皆無) | 2026-02-07 |
| TI-06 | スレッドモデル | 回答済み | 2026-02-07 |
| TI-07 | IPC パイプ帯域と欠落率 | 回答済み (WSL2 + Native) | 2026-02-08 |
| TI-08 | IPC ボトルネック再評価と次ステップ判定 | 回答済み | 2026-02-08 |

---

## TI-01: GPU 転送ボトルネック

> PCIe 帯域に対して Vulkan vkCmdCopyBuffer の実効スループットはどこまで出るか。Staging Buffer のサイズと転送頻度のトレードオフ。

### 2026-02-07 Phase 3-4 (llvmpipe, WSL2, Ryzen 9 9950X3D)

**計測結果 (BM-A)**

| バッファサイズ | スループット | 備考 |
|---|---|---|
| 1 MB | 11.4 GB/s | オーバーヘッド比率大 |
| 4 MB | 31.6 GB/s | ほぼ飽和 |
| 16 MB | 35.9 GB/s | ピーク |
| 64 MB | 17.4 GB/s | キャッシュミス/TLB ミスで低下 |

**分析**

- **注意**: llvmpipe 環境では vkCmdCopyBuffer は CPU メモリ間コピー (memcpy) として実行されるため、計測値は PCIe 帯域ではなく **メモリ帯域** を反映している。実 GPU 環境では PCIe Gen3 x16 (理論 ~16 GB/s)、Gen4 (理論 ~32 GB/s) が上限となる。
- **Staging バッファ戦略**: 間引き後の転送データは 3840 vtx × 2 bytes = 7.68 KB/フレーム。この微小サイズでは PCIe 帯域は全く問題にならない。**CPU 側間引きにより転送量を 1/10,000 以下に削減する設計は正しい。**

**結論**: GPU 転送はボトルネックではない。実ハードウェア検証を推奨。

### 2026-02-07 Phase 6 (RTX 5080 via dzn, WSL2, Release ビルド)

**計測結果 (BM-A)**

| バッファサイズ | スループット | llvmpipe比 | 備考 |
|---|---|---|---|
| 1 MB | 4.8 GB/s | 0.42x | D3D12 変換レイヤーのオーバーヘッド |
| 4 MB | 12.0 GB/s | 0.38x | |
| 16 MB | 20.1 GB/s | 0.56x | |
| 64 MB | 24.4 GB/s | 1.40x | 大サイズで優位 |

**分析**

- dzn ドライバは Vulkan コマンドを D3D12 に変換して実行するため、per-call のオーバーヘッドが大きい。小バッファでは llvmpipe (純粋な memcpy) に劣る。
- 64 MB で逆転するのは、実際の GPU メモリ転送 (D3D12 CopyBufferRegion) が CPU memcpy より効率的なため。
- **重要**: 間引き後の転送は 7.68 KB/フレームのため、転送スループットは全く問題にならないことが llvmpipe に続き実 GPU でも確認された。
- PCIe Gen5 x16 の理論帯域 (~63 GB/s) には未到達。dzn の D3D12 変換オーバーヘッドが支配的。

**結論 (更新)**: GPU 転送はボトルネックではない。**実 GPU 環境で確認済み。**

### 2026-02-07 Phase 7 (RTX 5080 Native NVIDIA Vulkan, Windows, MSVC Release)

**計測結果 (BM-A)**

| バッファサイズ | スループット | dzn比 | 備考 |
|---|---|---|---|
| 1 MB | 10.3 GB/s | 2.2x | ネイティブドライバで小バッファ改善 |
| 4 MB | 18.3 GB/s | 1.5x | |
| 16 MB | 22.2 GB/s | 1.1x | |
| 64 MB | 23.7 GB/s | ~同等 | 大サイズは同等 |

**分析**

- ネイティブ NVIDIA Vulkan ドライバで小バッファ (1MB) の転送が 2.2x 改善。D3D12 変換レイヤーの per-call オーバーヘッドが排除された効果。
- 大バッファ (64MB) では dzn とほぼ同等 — 大サイズでは D3D12 変換のオーバーヘッドは転送時間に対して無視できる。
- 間引き後の 7.68 KB/フレームに対しては依然として全く問題にならないことを再確認。

**結論 (最終)**: GPU 転送はボトルネックではない。llvmpipe、dzn、ネイティブ Vulkan の **3 環境すべてで確認済み**。

---

## TI-02: CPU 間引き性能

> MinMax の SIMD 最適化でどこまで出るか。LTTB は 1 GSPS に対して実用的か。マルチスレッド並列間引きの効果。

### 2026-02-07 Phase 3-4 (llvmpipe, WSL2, Ryzen 9 9950X3D)

**計測結果 (BM-B)**

| アルゴリズム | スループット | 1 GSPS 余裕 |
|---|---|---|
| MinMax (scalar) | 1,354 MSPS | 1.35x |
| MinMax (SSE2 SIMD) | 1,526 MSPS | 1.53x |
| LTTB | 213 MSPS | 0.21x (**不足**) |

**分析**

- **MinMax SIMD は 1 GSPS を処理可能** (1.5x マージン)。AVX2 化でさらなる向上が見込める。
- **LTTB は 100 MSPS 以上で実用不可**。O(n) ではあるが分岐が多く、SIMD 化が困難。高レートでは MinMax 一択。
- **SIMD の効果が限定的** (1.13x): llvmpipe の LLVM バックエンドが scalar コードを自動ベクトル化している可能性。実 CPU + 実 GPU 環境では手動 SIMD の効果が拡大する可能性がある。
- **マルチスレッド並列間引き**: 本 PoC では未実装。1 GSPS で 1.5x マージンがあるため、現時点では不要。2 GSPS 以上を目指す場合に検討。

**結論**: MinMax SIMD で 1 GSPS は達成可能。LTTB は高レートに不適。

### 2026-02-07 Phase 6 (Release ビルド, Ryzen 9 9950X3D)

**計測結果 (BM-B, Release ビルド)**

| アルゴリズム | スループット | 1 GSPS 余裕 | Debug比 |
|---|---|---|---|
| MinMax (scalar) | 19,988 MSPS | 20.0x | 14.8x |
| MinMax (SSE2 SIMD) | 21,521 MSPS | 21.5x | 14.1x |
| LTTB | 734 MSPS | 0.73x (**不足**) | 3.4x |

**分析**

- **Release ビルド (-O2) で劇的に性能向上**: scalar MinMax が 14.8 倍、SIMD が 14.1 倍。Debug ビルドの計測値は最適化が無効だったため、実際の性能を大幅に下回っていた。
- **MinMax SIMD は 21.5x マージン**: 1 GSPS に対して圧倒的な余裕。AVX2 最適化の必要性はさらに低下。
- **SIMD vs scalar の差が縮小** (1.08x): GCC -O2 の自動ベクトル化が scalar コードを効果的に最適化していることが判明。
- **LTTB は Release でも 734 MSPS**: 1 GSPS には不足だが、100 MSPS では 7.3x マージンがあり、Phase 5 の ≥100 MSPS 自動無効化閾値は妥当。

**結論 (更新)**: Release ビルドで MinMax は 21 GSPS 相当のスループット。1 GSPS に対して 21x マージン。

### 2026-02-07 Phase 7 (MSVC Release, Ryzen 9 9950X3D)

**計測結果 (BM-B, MSVC Release)**

| アルゴリズム | スループット | 1 GSPS 余裕 | GCC比 |
|---|---|---|---|
| MinMax (scalar) | 1,884 MSPS | 1.9x | 0.09x |
| MinMax (SSE2 SIMD) | 19,834 MSPS | 19.8x | 0.92x |
| LTTB | 743 MSPS | 0.74x | 1.01x |

**分析**

- **MSVC と GCC の auto-vectorize 差が顕著**: GCC -O2 は scalar MinMax ループを自動ベクトル化し 19,988 MSPS を達成するが、MSVC /O2 は auto-vectorize しないため 1,884 MSPS に留まる。
- **手動 SIMD の真の効果が判明**: MSVC では SIMD/scalar = **10.5x** の高速化率。GCC の 1.1x は auto-vectorize により scalar がすでに SIMD 化されていたため。手動 SSE2 intrinsics の実効性が確認された。
- **SIMD 実装同士ではほぼ同等**: GCC 21,521 vs MSVC 19,834 MSPS (0.92x)。SIMD intrinsics を直接使用する場合、コンパイラ差は小さい。
- **LTTB は完全に同等**: GCC 734 vs MSVC 743 MSPS。分岐の多いアルゴリズムでは auto-vectorize の影響が小さい。

**結論 (更新)**: MSVC でも MinMax SIMD は 19.8 GSPS 相当。**コンパイラに依存しない安定した性能を確認**。GCC の auto-vectorize に頼らず、明示的 SIMD 実装が正解。

---

## TI-03: GPU 側間引きの可能性

> Compute Shader で間引きを行った場合の性能差。CPU 間引き vs GPU 間引きどちらが速いか。

### 2026-02-07 Phase 3-4 (llvmpipe, WSL2, Ryzen 9 9950X3D)

**計測結果 (BM-E)**

| 方式 | スループット | 備考 |
|---|---|---|
| CPU MinMax (SIMD) | 1,526 MSPS | SSE2, 16M samples → 3840 vtx |
| GPU Compute MinMax | 472 MSPS | 256 threads/workgroup, shared memory reduction |

**分析**

- **CPU が 3.2 倍高速** (llvmpipe 環境)。
- 理由:
  1. llvmpipe はソフトウェアレンダラであり、compute shader のディスパッチオーバーヘッドが大きい
  2. CPU→GPU→CPU のデータ往復コスト (int16→int32 変換 + readback) が支配的
  3. 実 GPU 環境では GPU compute が逆転する可能性あり (特に PCIe Direct Storage/ReBAR 環境)
- **重要な設計上の知見**: CPU 側で間引いた結果 (3840 vtx = 7.68 KB) だけを GPU に送る方が、raw データ (16M samples = 32 MB) を GPU に送って GPU 側で間引くより**転送量が 4,000 倍少ない**。PCIe が律速になる環境では CPU 間引きが圧倒的に有利。

**結論**: CPU 側間引き + 小データ転送が現行最適解。GPU compute は実ハードウェアで再検証を推奨。

### 2026-02-07 Phase 6 (RTX 5080 via dzn, WSL2, Release ビルド)

**計測結果 (BM-E)**

| 方式 | スループット | llvmpipe比 | 備考 |
|---|---|---|---|
| CPU MinMax (SIMD) | 21,521 MSPS | 14.1x | Release ビルド最適化 |
| GPU Compute MinMax | 3,032 MSPS | 6.4x | RTX 5080 (D3D12 経由) |

**分析**

- **CPU SIMD が依然として 7.1 倍高速** (21,521 vs 3,032 MSPS)。
- GPU Compute は llvmpipe 比で 6.4x 向上 (472 → 3,032 MSPS)。実 GPU の compute ユニットが活用されている。
- ただし CPU 側も Release 最適化で 14.1x 向上しており、比率は llvmpipe 時の 3.2x → 7.1x と **CPU 優位がさらに拡大**。
- dzn の D3D12 変換オーバーヘッド (5.6 ms/call) がボトルネック。ネイティブ Vulkan ドライバでは GPU compute がさらに高速になる可能性がある。
- **設計上の知見は不変**: CPU 間引き (7.68 KB) vs GPU 間引き (32 MB 転送) の転送量差 4,000x は GPU compute の速度差では補えない。

**結論 (更新)**: CPU 側間引き + 小データ転送が最適であることを **実 GPU 環境でも確認**。GPU compute は dzn オーバーヘッドを除いても CPU SIMD に対する優位性が薄い。

### 2026-02-07 Phase 7 (RTX 5080 Native NVIDIA Vulkan, Windows, MSVC Release)

**計測結果 (BM-E)**

| 方式 | スループット | dzn比 | 備考 |
|---|---|---|---|
| CPU MinMax (SIMD) | 19,834 MSPS | 0.92x | MSVC ビルド |
| GPU Compute MinMax | 5,127 MSPS | 1.69x | ネイティブ Vulkan、3.2 ms/call |

**分析**

- **ネイティブ Vulkan で GPU Compute が 1.7x 向上** (3,032 → 5,127 MSPS): D3D12 変換オーバーヘッド排除の効果。
- **それでも CPU SIMD が 3.9x 高速** (19,834 vs 5,127 MSPS): GPU compute のオーバーヘッド (dispatch + readback) はネイティブドライバでも無視できない。
- **CPU 間引き設計の最終確認**: llvmpipe (3.2x)、dzn (7.1x)、ネイティブ (3.9x) のすべてで CPU SIMD が GPU compute を上回る。**CPU 側間引きが最適であることが 3 環境で一貫して確認された。**

**結論 (最終)**: CPU 側間引き + 小データ転送が最適。**3 実行環境すべてで確認済み。**

---

## TI-04: 描画プリミティブ選択

> LINE_STRIP vs 他のプリミティブの描画コスト。頂点数と描画パフォーマンスの関係。

### 2026-02-07 Phase 3-4 (llvmpipe, WSL2, Ryzen 9 9950X3D)

**計測結果 (BM-C, V-Sync OFF)**

| 頂点数 | FPS | frame_ms | 備考 |
|---|---|---|---|
| 3,840 | 470 | 2.1 ms | MinMax 間引き後の標準頂点数 |
| 38,400 | 289 | 3.5 ms | 10x |
| 384,000 | 101 | 9.9 ms | 100x |

**分析**

- **LINE_STRIP は十分高速**: 3840 頂点で 470 FPS (2.1ms/frame)。V-Sync 16.67ms に対して 8 倍の余裕。
- **描画コストのスケーリング**: ほぼ線形 (3.8K→384K で約 5x のコスト増)。
- **律速となる頂点数の目安**: ≈800K 頂点で 16.67ms に到達すると推定 (V-Sync 60 FPS 限界)。
- **他プリミティブ未検証**: Instanced Quad (太線) や GL_POINTS は未実装。MinMax 後の 3840 頂点では LINE_STRIP の性能で十分であり、追加検証の優先度は低い。

**結論**: LINE_STRIP で十分。間引きにより頂点数が一定 (3840) なため、描画は律速にならない。

### 2026-02-07 Phase 6 (RTX 5080 via dzn, WSL2, Release ビルド)

**計測結果 (BM-C, V-Sync OFF)**

| 頂点数 | FPS | frame_ms | llvmpipe比 |
|---|---|---|---|
| 3,840 | 770 | 1.3 ms | 1.6x |
| 38,400 | 797 | 1.3 ms | 2.8x |
| 384,000 | 838 | 1.2 ms | 8.3x |

**分析**

- **RTX 5080 では頂点数に関係なく ~770-838 FPS**: 頂点処理は全く律速にならない。llvmpipe と異なり線形スケーリングが観察されない。
- **フレーム時間のフロア ~1.2-1.3ms**: dzn の D3D12 変換レイヤー + コマンド発行のオーバーヘッドが支配的と推定。
- **L3 計測値**: 3840 頂点で **770 FPS** (1.3ms/frame)。V-Sync 16.67ms の **13 倍の余裕**。
- 384K 頂点でも 838 FPS と高速なのは、RTX 5080 の頂点処理能力が圧倒的なため。

**結論 (更新)**: 実 GPU では描画性能に圧倒的余裕がある。間引き後 3840 頂点で 770 FPS、V-Sync 60fps に対し 13x マージン。

### 2026-02-07 Phase 7 (RTX 5080 Native NVIDIA Vulkan, Windows, MSVC Release)

**計測結果 (BM-C, V-Sync OFF)**

| 頂点数 | FPS | frame_ms | dzn比 |
|---|---|---|---|
| 3,840 | 2,022 | 0.49 ms | **2.6x** |
| 38,400 | 3,909 | 0.26 ms | **4.9x** |
| 384,000 | 3,741 | 0.27 ms | **4.5x** |

**分析**

- **ネイティブ Vulkan で劇的改善**: dzn の 1.2-1.3ms/frame のフロアが 0.26-0.49ms に低下。D3D12 変換オーバーヘッドが排除された。
- **3840 頂点で 2,022 FPS**: V-Sync 16.67ms に対して **34 倍の余裕**。dzn の 13x から大幅に拡大。
- **逆転現象の解消**: dzn では 384K > 38K > 3.8K (逆転) だったが、ネイティブでは 38K > 384K > 3.8K と、頂点処理の GPU 並列性が正しく活用されている。
- **L3 更新**: **2,022 FPS** (ネイティブ NVIDIA Vulkan, 3840 vtx)。

**結論 (最終)**: ネイティブ Vulkan で **2,022 FPS** (34x マージン)。描画は全くボトルネックにならない。

---

## TI-05: 永続マップドバッファ

> ReBAR/SAM 対応環境での HOST_VISIBLE + DEVICE_LOCAL 直接書き込みは Staging 経由より速いか。

### 2026-02-07 Phase 3-4 (llvmpipe, WSL2)

**計測結果**: 未計測。llvmpipe 環境では ReBAR/SAM 機能が利用できないため、本 PoC では検証不可。

**設計上の考察**:
- 現行の staging 経由転送は 3840 vtx × 2 bytes = 7.68 KB/フレームと極めて小さい
- この転送量では staging vs 永続マップのどちらでも差は無視できるレベル
- ReBAR の効果が出るのは大データ転送時 (数十 MB 以上)。間引き後の転送には適用理由が薄い
- raw データを GPU に直接転送して GPU 側で間引く構成 (TI-03 関連) では ReBAR の効果が大きい可能性あり

**結論**: 現行設計では優先度低。GPU 側間引き構成を採用する場合に再検討。

### 2026-02-07 Phase 6 (RTX 5080 via dzn, WSL2)

**計測結果**: 検証不可。Mesa dzn ドライバは D3D12→Vulkan 変換レイヤーであり、ReBAR/SAM (HOST_VISIBLE + DEVICE_LOCAL 統合メモリ) の制御を行わない。

**追加考察**:
- Phase 6 の BM-A で確認された通り、間引き後の転送量は 7.68 KB/フレーム
- この微小サイズでは staging 経由でも永続マップでも転送時間は < 0.5ms と無視できる
- ReBAR が効果を発揮するのは数十 MB 以上の転送時のみであり、現行の CPU 間引き設計では恩恵が皆無
- 本項目の検証には Windows ネイティブ環境 + ネイティブ Vulkan ドライバが必要

**結論 (更新)**: dzn 環境では検証不可。ただし現行設計 (CPU 間引き→小データ転送) では ReBAR の恩恵が皆無であり、**検証の実用的価値は極めて低い**。

---

## TI-06: スレッドモデル

> 生成・間引き・転送・描画の分離粒度の最適解。lock-free ring buffer vs mutex-guarded double buffer。

### 2026-02-07 Phase 3-4 (llvmpipe, WSL2, Ryzen 9 9950X3D)

**計測結果 (Profile データ)**

| 指標 | 1 GSPS 計測値 | 備考 |
|---|---|---|
| ring_fill avg | 0.02% | ほぼ空 — 消費が生成に追従 |
| ring_fill max | 0.3% | 瞬間的な spike のみ |
| drain_ms | 0.0005 ms | ダブルバッファ swap のみ |
| decimate_ms | 0.21 ms | 間引きスレッドでの処理 |

**分析**

- **現行の 3 スレッドモデルが有効**:
  1. **生成スレッド**: period tiling (memcpy) で 1 GSPS を維持。busy-wait pacing で正確なレート制御。
  2. **間引きスレッド**: ring buffer drain + MinMax + ダブルバッファ出力。フレームごとの処理時間 0.21ms。
  3. **メインスレッド (描画)**: upload + render + HUD。V-Sync 待ちが大半。
- **lock-free SPSC ring buffer の有効性**:
  - ring_fill が常時 <1% であり、生産者・消費者間にコンテンションなし
  - mutex-guarded 方式と比較して、lock-free は高レートでのレイテンシ変動を抑制
  - 1 GSPS でのスループット維持に貢献
- **改善の余地**:
  - 4 スレッド化 (生成/間引き/転送/描画) は現状不要 — upload_ms が 0.04ms と微小
  - 間引きスレッドの sleep (100μs) をイベント駆動に変更すれば latency を削減可能

**結論**: 現行 3 スレッドモデル + lock-free SPSC が最適。4 スレッド化の必要性なし。

---

## TI-07: IPC パイプ帯域と欠落率

> Anonymous pipe IPC (`grebe-sg` → `grebe`) の実効帯域はどこまで出るか。欠落率 (drop rate) はどの入力レートで発生するか。Embedded モードとの性能差。

### 2026-02-08 Phase 9 (dzn, WSL2, GCC Debug ビルド, Ryzen 9 9950X3D)

**計測条件**

- `--profile` による自動シナリオ (1M/10M/100M/1G SPS × warmup 120 + measure 300 frames)
- Ring buffer: 16M samples/channel, Block size: 4096 samples
- V-Sync ON (60 FPS ターゲット)
- IPC プロトコル: FrameHeaderV2 (48 bytes) + channel-major int16 payload via anonymous pipe (stdout)

**計測結果 (1ch)**

| シナリオ | モード | FPS avg | FPS min | Smp/frame | Drops | 備考 |
|---|---|---|---|---|---|---|
| 1 MSPS | IPC | 60.1 | 58.7 | 4,096 | 0 | パイプ帯域に余裕あり |
| 1 MSPS | Embedded | 60.0 | 58.5 | 4,096 | 0 | 基準 |
| 10 MSPS | IPC | 60.0 | 40.0 | 4,096 | 0 | |
| 10 MSPS | Embedded | 60.0 | 58.5 | 4,096 | 0 | |
| 100 MSPS | IPC | 59.9 | 57.9 | 51,594 | 0 | IPC で配信量低下 |
| 100 MSPS | Embedded | 60.0 | 58.4 | 65,536 | 0 | smp/f 1.27x |
| 1 GSPS | IPC | 58.7 | 57.3 | 83,118 | 0 | パイプ帯域が律速 |
| 1 GSPS | Embedded | 60.0 | 58.4 | 147,499 | 0 | smp/f 1.77x |

**計測結果 (4ch)**

| シナリオ | モード | FPS avg | FPS min | Smp/frame | Drops | 備考 |
|---|---|---|---|---|---|---|
| 4ch×1 MSPS | IPC | 60.1 | 58.5 | 16,204 | 0 | |
| 4ch×1 MSPS | Embedded | 60.0 | 58.3 | 16,104 | 0 | |
| 4ch×10 MSPS | IPC | 60.0 | 58.0 | 16,384 | 0 | |
| 4ch×10 MSPS | Embedded | 60.0 | 58.4 | 15,853 | 0 | |
| 4ch×100 MSPS | IPC | 59.4 | 57.9 | 110,078 | 0 | パイプ帯域低下開始 |
| 4ch×100 MSPS | Embedded | 60.0 | 58.5 | 240,822 | 0 | smp/f 2.19x |
| 4ch×1 GSPS | IPC | 56.7 | 54.8 | 114,282 | 35,184,640 | **欠落発生** |
| 4ch×1 GSPS | Embedded | 57.3 | 54.6 | 60,333,471 | 2,447,638,528 | 両モードで飽和 |

**IPC 実効帯域の推定**

理論パイプ帯域: `(48 + block_size × channels × 2) bytes/frame × (sample_rate / block_size) frames/sec`

| シナリオ | 理論帯域 (MB/s) | 実効 smp/frame (IPC) | 実効帯域 (MB/s) 推定 | 備考 |
|---|---|---|---|---|
| 1ch × 1 MSPS | 2.0 | 4,096 | 2.0 | 100% 配信 |
| 1ch × 100 MSPS | 195 | 51,594 | 152 | 78% |
| 1ch × 1 GSPS | 1,953 | 83,118 | 245 | パイプ飽和 |
| 4ch × 100 MSPS | 781 | 110,078 | 325 | 42% |
| 4ch × 1 GSPS | 7,813 | 114,282 | 337 | パイプ飽和 |

推定実効パイプ帯域: **約 250-340 MB/s** (WSL2 anonymous pipe, 4096 byte ブロック)

**分析**

1. **パイプ帯域の上限**: WSL2 の anonymous pipe は約 250-340 MB/s で飽和する。1ch×100 MSPS (理論 195 MB/s) まではほぼ 100% 配信可能だが、それ以上ではパイプが律速となりサンプル配信量が低下する。

2. **欠落の発生条件**: 4ch×1 GSPS (理論 7.8 GB/s) でのみ IPC 側で欠落が発生 (35M drops)。ただし embedded モードでも同シナリオで大量欠落 (2.4G drops) が発生しており、これはリングバッファのオーバーフロー (DataGenerator の生成レートがデシメーションスレッドの消費レートを上回る) が主因。

3. **FPS への影響**: IPC オーバーヘッドによる FPS 低下は小さい (1 GSPS でも embedded 60.0 vs IPC 58.7 FPS)。パイプ帯域が律速になってもフレームレートへの影響は限定的 — サンプル配信量が減るだけで、デシメーション後の頂点数 (3840/ch) は変わらないため描画コストは不変。

4. **Embedded モードとの比較**: 100 MSPS 以下ではほぼ同等の性能。100 MSPS 以上では samples_per_frame の差が拡大するが、可視化品質への影響は限定的 (MinMax デシメーション後は同一頂点数)。

5. **ブロックサイズの影響**: 現在のブロックサイズ 4096 samples ではヘッダーオーバーヘッドは 48/(48+8192) = 0.58% と小さい。ブロックサイズ拡大 (e.g. 16384) でスループットが向上する可能性があるが、レイテンシとのトレードオフ。

**結論**: Anonymous pipe IPC は 100 MSPS/1ch まで無欠落で動作し、実用上十分。1 GSPS では パイプ帯域 (~300 MB/s) が律速となるが FPS への影響は軽微。高帯域が必要な場合は shared memory IPC (Phase 10) への移行を推奨。

### 2026-02-08 Phase 9 (RTX 5080 Native NVIDIA Vulkan, Windows, MSVC Release)

**計測条件**

- `--profile` による自動シナリオ (1M/10M/100M/1G SPS × warmup 120 + measure 300 frames)
- Ring buffer: 64M samples/channel, Block size: 4096 samples
- V-Sync ON (60 FPS ターゲット)
- ネイティブ NVIDIA Vulkan ドライバ、MSVC Release ビルド
- Windows 版 `PipeProducer::receive_command()` を `PeekNamedPipe` で実装済み (R-12 解消)

**注記**: Windows ネイティブ Vulkan + V-Sync 環境では一部フレームに sub-ms の frame_time が発生し、FPS avg 統計に外れ値が含まれる。以下の表では FPS avg の代わりに frame_ms avg から算出した実効 FPS (≈ 1000/frame_ms) を併記する。

**計測結果: IPC モード (1ch)**

| シナリオ | frame_ms | 実効FPS | Smp/frame | Drops | 備考 |
|---|---|---|---|---|---|
| 1 MSPS | 16.70 | 59.9 | 12,636 | 0 | パイプ帯域に余裕 |
| 10 MSPS | 16.65 | 60.1 | 95,732 | 0 | レート変更動作確認 |
| 100 MSPS | 16.65 | 60.1 | 111,532 | 0 | パイプ帯域が律速開始 |
| 1 GSPS | 16.64 | 60.1 | 7,875 | 0 | パイプ飽和 + 生成側制約 |

**計測結果: IPC モード (4ch)**

| シナリオ | frame_ms | 実効FPS | Smp/frame | Drops | 備考 |
|---|---|---|---|---|---|
| 4ch×1 MSPS | 16.65 | 60.1 | 59,217 | 0 | |
| 4ch×10 MSPS | 16.66 | 60.0 | 298,481 | 0 | 4ch で帯域効率向上 |
| 4ch×100 MSPS | 16.64 | 60.1 | 75,725 | 0 | パイプ飽和 |
| 4ch×1 GSPS | 16.55 | 60.4 | 56,884 | 0 | パイプ飽和 |

**計測結果: Embedded モード (1ch)**

| シナリオ | frame_ms | 実効FPS | Smp/frame | Drops | WSL2比 |
|---|---|---|---|---|---|
| 1 MSPS | 16.62 | 60.2 | 4,096 | 0 | 同等 |
| 10 MSPS | 16.63 | 60.1 | 13,668 | 0 | 同等 |
| 100 MSPS | 16.63 | 60.1 | 148,588 | 0 | 2.27x smp/f |
| 1 GSPS | 16.60 | 60.2 | 157,825 | 0 | 1.07x smp/f |

**計測結果: Embedded モード (4ch)**

| シナリオ | frame_ms | 実効FPS | Smp/frame | Drops | WSL2比 |
|---|---|---|---|---|---|
| 4ch×1 MSPS | 16.62 | 60.2 | 16,048 | 0 | 同等 |
| 4ch×10 MSPS | 16.62 | 60.2 | 50,662 | 0 | 3.20x smp/f |
| 4ch×100 MSPS | 16.62 | 60.2 | 529,123 | 0 | 2.20x smp/f |
| 4ch×1 GSPS | 16.58 | 60.3 | 1,871,336 | 0 | **0 drops** (WSL2: 2.4G drops) |

**IPC 実効帯域の推定 (Windows native)**

| シナリオ | Smp/frame (IPC) | 推定帯域 (MB/s) | WSL2比 | 備考 |
|---|---|---|---|---|
| 1ch × 10 MSPS | 95,732 | 11.5 | 0.04x | |
| 1ch × 100 MSPS | 111,532 | 13.4 | 0.09x | パイプ飽和 |
| 4ch × 10 MSPS | 298,481 | 35.8 | — | 4ch で効率改善 |
| 4ch × 100 MSPS | 75,725 | 9.1 | 0.03x | パイプ飽和 |

推定実効パイプ帯域: **約 10-36 MB/s** (Windows ネイティブ anonymous pipe, 4096 byte ブロック)

**分析**

1. **Windows パイプ帯域は WSL2 の 1/10〜1/30**: WSL2 anonymous pipe の実効帯域 (~300 MB/s) に対し、Windows ネイティブは **~10-36 MB/s** と大幅に低い。Windows の anonymous pipe はデフォルトバッファサイズが 4,096 bytes (Linux は 65,536 bytes) であり、per-write のシステムコールオーバーヘッドが大きい。

2. **全シナリオ 0 drops**: パイプ帯域が低いため grebe 側リングバッファへの流入が緩やかになり、結果的に全シナリオで欠落ゼロ。WSL2 で発生した 4ch×1GSPS のドロップ (35M drops) が Windows では解消されている。

3. **Embedded 4ch×1 GSPS の劇的改善**: WSL2 dzn では 2,447,638,528 drops だったが、Windows ネイティブでは **0 drops**。smp/f は 1,871,336 (WSL2: 60,333,471) と低いが、DataGenerator のスレッドスケジューリング特性 (Windows の busy-wait pacing がより保守的) によりリングバッファのオーバーフローを回避。

4. **Render time の改善**: ネイティブ Vulkan で render_ms ~11ms (WSL2 dzn: ~16ms)。dzn の D3D12 変換オーバーヘッド排除により描画パイプラインに余裕が拡大。FPS は 60 で同等だが、CPU 余力が増加。

5. **パイプ最適化の必要性**: Windows ネイティブの低パイプ帯域は Phase 10 (Pipe 最適化) の主要ターゲット。`CreatePipe` のバッファサイズ拡大 (4KB → 64KB+) と write batching により大幅な改善が見込める。

**結論 (更新)**: Windows ネイティブの anonymous pipe 帯域は WSL2 比で大幅に低い (~10-36 MB/s vs ~300 MB/s)。ただし FPS への影響は皆無 (全シナリオ 60 FPS, 0 drops)。可視化品質は十分だが、高レートでの smp/frame が低下する。Phase 10 でのパイプバッファ最適化で改善が期待できる。

### 2026-02-08 Phase 10: Pipe IPC 最適化後

**最適化内容**

1. **パイプバッファ拡大**: Windows `CreatePipe` バッファ 0 (=4KB) → 1MB、Linux `fcntl(F_SETPIPE_SZ)` 64KB → 1MB
2. **デフォルトブロックサイズ変更**: 4096 → 16384 samples/channel/frame (`--block-size=N` CLI 追加)
3. **writev 最適化 (Linux)**: header+payload を 1 回のシステムコールで送信

**計測条件**

- `--profile` 自動シナリオ (1M/10M/100M/1G SPS × warmup 120 + measure 300 frames)
- Ring buffer: 64M samples/channel, V-Sync ON (60 FPS ターゲット)
- ブロックサイズ: 16384 (デフォルト) と 65536 の 2 パターン計測

**WSL2 計測結果 (dzn, GCC Debug)**

| シナリオ | block=4096 (Phase 9) | block=16384 | block=65536 | 改善率 (16K) |
|---|---|---|---|---|
| 1ch×100M | smp/f=51,594 | 59,566 | 65,536 | 1.15x |
| 1ch×1G | smp/f=83,118 | 138,434 | 189,231 | 1.67x |
| 4ch×100M | smp/f=110,078 | 191,884 | 255,976 | 1.74x |
| 4ch×1G | smp/f=114,282 (35M drops) | 18,957,171 (1.06G drops) | 45,293,051 (2.58G drops) | >>10x |

- 全シナリオ 60 FPS 維持、embedded baseline リグレッションなし
- 4ch×1G ではパイプ帯域の制約が解消され、ring buffer overflow がボトルネックに移行

**Windows ネイティブ計測結果 (NVIDIA Vulkan, MSVC Release)**

| シナリオ | block=4096 (Phase 9) | block=16384 | block=65536 | 改善率 (16K) |
|---|---|---|---|---|
| 1ch×10M | smp/f=95,732 | 133,508 | 160,938 | 1.4x |
| 1ch×100M | smp/f=111,532 | 694,261 | 1,310,647 | 6.2x |
| 1ch×1G | smp/f=7,875 | 946,959 | 3,957,748 | 120x |
| 4ch×10M | smp/f=298,481 | 622,380 | 592,208 | 2.1x |
| 4ch×100M | smp/f=75,725 | 2,257,173 | 1,175,115 | 29.8x |
| 4ch×1G | smp/f=56,884 (0 drops) | 49,220,018 (5.29G drops) | 1,886,708 (0 drops) | 865x |

- 全シナリオ 60 FPS 維持
- **Windows 帯域が 1 桁〜2 桁改善**: `CreatePipe` バッファ 1MB 化が最大の効果

**実効帯域推定 (Phase 10 block=16384)**

| 環境 | 1ch×1G smp/f | 推定帯域 (MB/s) | Phase 9 比 |
|---|---|---|---|
| WSL2 | 138,434 | ~410 | 1.67x (245→410) |
| Windows | 946,959 | ~114 | ~8x (10-36→114) |

**分析**

1. **Windows の劇的改善**: `CreatePipe` バッファ 4KB → 1MB により、Windows ネイティブの IPC 帯域が ~10 MB/s → ~100-470 MB/s に改善。ブロックサイズ 65536 では 1ch×1G で smp/f が 500x 以上向上。
2. **WSL2 でも改善**: パイプバッファ 1MB + writev により 1.5-2x の帯域改善。ただし 4ch×1G ではパイプが解消した代わりにリングバッファ overflow が顕在化。
3. **ブロックサイズの効果**: 16384 → 65536 でさらに帯域が向上するが、4ch×1G では挙動が異なる (16384: 大量 drops 発生 vs 65536: 0 drops)。大きいブロックは sender thread のレート制御に影響。
4. **ボトルネック移行**: Windows では pipe が律速でなくなり、DataGenerator の生成レートや ring buffer サイズがボトルネックに。WSL2 4ch×1G でも同様の移行が発生。

**Go/No-Go 判定**

- **基準**: Windows ネイティブで 100 MB/s 以上 → Phase 11 延期可能
- **結果**: 1ch×1G block=16384 で ~114 MB/s、block=65536 で ~475 MB/s → **100 MB/s を超過**
- **判定: Phase 11 (Shared Memory) 延期**。現行 pipe 最適化で実用上十分な帯域を確保。

**結論 (Phase 10)**: パイプバッファ拡大 (1MB) とブロックサイズ増 (16384) で、特に Windows ネイティブの IPC 帯域を大幅改善。Phase 9 で課題だった Windows の低帯域 (~10-36 MB/s) は解消され、100 MB/s 以上を達成。Phase 11 (Shared Memory) の即時実装は不要と判定。将来的に 4ch×1GSPS の ring buffer overflow 対策が必要な場合に shm を再検討。

---

## TI-08: IPC ボトルネック再評価と次ステップ判定

> Phase 10 の pipe 最適化結果から残存ボトルネックを特定し、shm 実装 vs pipe/buffer 改善のどちらが本質的な問題に対する最適なアプローチかを評価する。

### 2026-02-08 Phase 10-2 (WSL2 dzn + Windows Native)

**背景と仮説**

Phase 10 でパイプ帯域は大幅に改善されたが、4ch×1GSPS で大量の drops が残存。

初期仮説: 「4ch×1G の drops は decimation thread のスループット不足 (Debug ~1.5 GSPS vs 4 GSPS 要求) が根本原因であり、Release ビルド (~21 GSPS) で解消される。transport 方式 (pipe/shm) は無関係。」

根拠: **WSL2 Embedded モード (パイプなし) の 4ch×1G で 2.4G drops 発生** — パイプが一切関与しない状態でもドロップが発生。

**1. ボトルネック定量分析: 間引きスループット vs 要求量**

| 構成 | BM-B 間引きスループット | 4ch×1G 要求量 | BM-B 余裕率 | 実パイプライン余裕率 |
|---|---|---|---|---|
| GCC Debug | ~1.5 GSPS | 4 GSPS | 0.38x (不足) | — |
| GCC Release | ~21.5 GSPS | 4 GSPS | 5.4x (余裕) | 0.94x (不足) |
| MSVC Release | ~19.8 GSPS (SIMD) | 4 GSPS | 5.0x (余裕) | — |

注: BM-B はキャッシュ暖機済みバッファ上の純アルゴリズム計測。実パイプラインはリングバッファ drain + キャッシュ冷えデータにより大幅に低下。

**2. WSL2 Release 検証結果 — 仮説の部分的棄却**

GCC Release ビルドで 4ch プロファイルを再計測。リングバッファ 64M、V-Sync ON。

| 構成 | 4ch×1G Drops | smp/f avg | decimate_ms avg | ring_fill avg/max | FPS |
|---|---|---|---|---|---|
| **Release Embedded** | 2,130,509,824 | 68,259,483 | 18.19 | 0.149 / 0.184 | 56.4 |
| **Release IPC block=16384** | 2,412,101,632 | 47,669,370 | 31.51 | 0.100 / 0.199 | 54.6 |
| **Release IPC block=65536** | 2,003,763,200 | 23,311,078 | 15.64 | 0.049 / 0.348 | 56.0 |
| Debug Embedded (Phase 9) | 2,447,638,528 | 60,333,471 | — | — | 57.3 |
| Debug IPC block=16384 | 1,063,124,992 | 18,957,171 | 11.56 | 0.061 / 0.207 | — |
| Debug IPC block=65536 | 2,582,249,472 | 45,293,051 | 28.83 | 0.093 / 0.208 | — |

**仮説検証結果: Release Embedded でも 2.13G drops が発生。Release vs Debug の改善は 13% のみ (2.45G → 2.13G)。初期仮説は棄却。**

**3. 実パイプラインスループット分析**

BM-B ベンチマーク (21 GSPS) と実パイプライン性能の乖離を定量化:

| 構成 | smp/f | decimate_ms | 実効スループット | BM-B 比 | 乖離要因 |
|---|---|---|---|---|---|
| Release Embedded | 68.3M | 18.19ms | 3.75 GSPS | 0.17x | ring drain + cache cold |
| Release IPC 16K | 47.7M | 31.51ms | 1.51 GSPS | 0.07x | + IPC receiver contention |
| Release IPC 64K | 23.3M | 15.64ms | 1.49 GSPS | 0.07x | + IPC receiver contention |

**根本原因: 実パイプラインのスループットは BM-B の 7-17% にまで低下する。** 主因:

1. **キャッシュ冷えデータ**: 4ch×1G で 1 フレームあたり ~136 MB のデータをリングバッファから読み出す。L3 キャッシュ (64 MB) を超過し、メモリ帯域が律速。BM-B はキャッシュ暖機済みのため乖離が生じる。
2. **リングバッファ drain オーバーヘッド**: pop_bulk の memcpy (circular buffer → linear buffer) でデータを二重読み。
3. **4ch 逐次処理**: 間引きスレッドは 4 チャンネルを逐次処理。チャンネル切替時にキャッシュラインが追い出される。
4. **IPC 受信スレッドとの競合**: IPC モードでは receiver thread が同じリングバッファに書き込み、cache line bouncing が発生。

**4. Embedded vs IPC の drop 比較 — パイプ起因 vs 消費側起因の切り分け**

| 比較 | 結論 |
|---|---|
| Embedded 2.13G ≈ IPC 64K 2.00G | パイプなし/ありで drops 同水準 → **transport は原因ではない** |
| IPC 16K (2.41G) > Embedded (2.13G) | burst 到着パターンがリング overflow を増加させる |
| Release (2.13G) ≈ Debug (2.45G) | Debug/Release 差は 13% → **アルゴリズム速度も主因ではない** |
| **共通要因**: メモリアクセスパターン | ring drain + cache cold + sequential 4ch → 全構成で同水準の drops |

**5. block_size 16384 vs 65536 の挙動差分析**

Windows Release:
- block=16384: 5.29G drops — pipe が高帯域で burst 配信 → リング一時的 overflow
- block=65536: 0 drops — sender thread の大ブロック送信が自然にレート抑制

WSL2 Release:
- block=16384: 2.41G (Embedded 2.13G より多い) — burst 到着がリング overflow を悪化
- block=65536: 2.00G (Embedded 2.13G より少ない) — ブロックサイズがパイプ帯域を自然に制限

**結論: ブロックサイズは sender のレート制御に間接的に影響する。** 大ブロックは送信間隔が長く、到着レートが平滑化される。小ブロックは burst 到着しやすく、リング overflow のリスクが増加。

**6. shm vs pipe/buffer 改善の投資対効果マトリクス**

| 観点 | shm 実装 | pipe (現行最適化済み) | 消費側改善 |
|---|---|---|---|
| **帯域** | メモリ帯域 (~40 GB/s) | ~400-470 MB/s | — |
| **現在のボトルネックへの効果** | **なし** (消費側が律速) | — | **直接解消** |
| **Embedded drops (2.13G) への効果** | なし (transport 無関係) | — | 解消可能 |
| **実装コスト** | 高 (OS API, 同期, 障害復旧) | 低 (完了済み) | 中 |
| **リスク** | 高 (WSL2 shm 制約, デバッグ困難) | 低 | 中 |
| **4ch×1G 0-drops 達成の蓋然性** | 低 (ボトルネック非該当) | — | 中-高 |

**7. 推奨次ステップ**

| 優先度 | 施策 | 期待効果 | コスト | 根拠 |
|---|---|---|---|---|
| 高 | Release ビルドでの計測を標準化 | Debug の人為的制約排除 | なし | TI-02 の 14x 乖離を計測から除外 |
| 中 | マルチスレッド間引き (4ch → 2+2 並列) | 4ch throughput 2x, cache 効率改善 | 中 | 実効 3.75 GSPS → ~7 GSPS で 4ch×1G 余裕 |
| 中 | リングバッファサイズ拡大 (64M → 256M) | burst overflow 吸収 | 低 | block=16K の burst overflow 対策 |
| 低 | adaptive block size | sender レート制御平滑化 | 中 | block=64K の 0-drops 挙動を自動化 |
| **低** | **shm 実装 (Phase 11)** | パイプ帯域制約排除 | **高** | **ボトルネック非該当のため投資対効果が低い** |

**判定**

- **shm 実装 (Phase 11): 引き続き延期。** ボトルネックが transport ではなく消費側 (ring drain + cache cold data) にあるため、shm で解決できる問題が存在しない。Embedded モードでも同水準の drops が発生することが決定的な証拠。
- **shm 再検討条件:** Release ビルドで pipe 帯域がボトルネックとなるユースケースが出現した場合。具体的には、消費側改善により実効スループットが pipe 帯域 (~400 MB/s ≈ 200 MSPS×2ch) を超過した場合。
- **4ch×1G の drops は PoC として許容可能。** FPS は 54-56 を維持し、MinMax デシメーション後の頂点数 (3840/ch) は不変。drops はデシメーション入力の時間窓が狭まるのみで、可視化品質への影響は軽微。

### Phase 10-3: パイプラインボトルネック解消

**実施施策:**

1. **マルチスレッド間引き** — `std::barrier` (C++20) による Two-phase 並列処理
   - Worker count: `min(channel_count, hardware_concurrency/2, 4)` — 1ch は従来シングルスレッド、4ch → 4 workers, 8ch → 4 workers
   - Phase 1 (並列): 各 worker が担当チャンネルの ring drain + decimate を並行実行
   - Phase 2 (逐次): coordinator がチャンネル順に結果を concatenate → front buffer swap
   - SPSC 安全性: 各 ring buffer は `ch % num_workers` で 1 worker のみが排他的に pop

2. **リングバッファデフォルト拡大** — 16M → 64M samples (CLI で 256M 指定可)

3. **Debug ビルド計測警告** — `--profile`/`--bench` を Debug で実行時に `spdlog::warn` を出力

**WSL2 Release 計測結果 (Phase 10-3):**

| 構成 | Drops | smp/f avg | decimate_ms avg | ring_fill avg | FPS |
|---|---|---|---|---|---|
| 1ch Embedded | 0 | 149,939 | — | 0.0% | 60.0 |
| 4ch Embedded (ring=256M) | 0 | 1,147,502 | 0.22-0.46 | 0.0-0.1% | 57.7 |
| 4ch IPC (ring=256M) | 0 | 649,813 | 0.20-0.39 | 0.0% | 55.7 |
| 8ch Embedded (ring=256M) | 0 | 1,709,750 | — | — | 57.2 |

**Phase 10-2 → 10-3 比較 (4ch Embedded):**

| 指標 | Phase 10-2 | Phase 10-3 | 改善 |
|---|---|---|---|
| Drops | 2,130,509,824 | **0** | ∞ |
| decimate_ms | 18.19 ms | 0.22-0.46 ms | **40-80x** |
| ring_fill avg | 14.9% | 0.0% | 完全解消 |
| FPS | 56.4 | 57.7 | +2.3% |
| Workers | 0 (single) | 4 (parallel) | — |

**分析:**

- マルチスレッド間引きにより decimate_ms が **40-80x 高速化**（18.19ms → 0.22-0.46ms）。4ch を並列処理することで、各 worker は 1ch 分のみ drain+decimate し、cache locality が大幅改善。
- ring_fill が 14.9% → 0.0% に低下。ring が一切蓄積せず、DataGenerator の全サンプルを即時消費。
- IPC モードの 4ch×1G も 0 drops。ただし smp/f (649K) は Embedded (1.15M) より低い — pipe 帯域が流入レートを制限するため。grebe-sg 側では DataGenerator → local ring の overflow (SG-side drops) が発生するが、grebe (viewer) 側のパイプライン処理は完全に追従。
- **8ch×1G も 0 drops** — 4 workers で 8ch を 2ch/worker ずつ処理。
- **shm 再検討条件の更新:** grebe 側の消費スループットが pipe 帯域を大幅に超過するようになったため、IPC mode で grebe-sg 側の SG-local drops を解消するには pipe 帯域の拡大 (shm) が有効になりうる。ただし Embedded モードでは完全 0-drops のため、PoC としての目標は達成済み。

**判定更新:**

- **4ch/8ch×1G 0-drops 達成。** Phase 10-2 の推奨施策 3 項目 (マルチスレッド間引き、リング拡大、Release 標準化) を全て実施し、Embedded モードで全レート・全チャンネル数で drops = 0 を達成。
- **shm (Phase 11) は引き続き延期。** IPC mode の grebe-sg 側 drops は残存するが、grebe (viewer) 側は 0-drops。Embedded モードが PoC のリファレンス動作。

---

## 推奨事項トラッカー

`doc/technical_judgment.md` で提起された推奨事項の対応状況を追跡する。

| # | 推奨事項 | 優先度 | ステータス | 対応Phase | 備考 |
|---|---|---|---|---|---|
| ~~R-01~~ | ~~実 GPU 環境での再計測~~ | ~~高~~ | ~~完了~~ | ~~Phase 6-7~~ | ~~dzn (Phase 6) + ネイティブ Vulkan (Phase 7) で全ベンチマーク計測済み~~ |
| ~~R-02~~ | ~~マルチチャンネル対応 (4ch/8ch)~~ | ~~高~~ | ~~完了~~ | ~~Phase 5~~ | ~~FR-07 実装済み (fb21264, 387c6fd)~~ |
| ~~R-03~~ | ~~LTTB の高レート無効化~~ | ~~高~~ | ~~完了~~ | ~~Phase 5~~ | ~~FR-02.6 実装済み (effective_mode_ パターン)~~ |
| R-04 | AVX2 MinMax 最適化 | 中 | 未着手 | — | Release ビルドで 21x マージン → 優先度低下 |
| ~~R-05~~ | ~~ReBAR/SAM 永続マップド検証~~ | ~~低~~ | ~~見送り~~ | ~~—~~ | ~~dzn 非対応 + 現設計で恩恵皆無 (TI-05 参照)~~ |
| ~~R-06~~ | ~~GPU Compute 間引きの実GPU再検証~~ | ~~中~~ | ~~完了~~ | ~~Phase 6~~ | ~~CPU SIMD 7.1x 高速 → CPU 間引き最適を再確認 (TI-03)~~ |
| R-07 | E2E レイテンシ計測 | 低 | 未着手 | — | NFR-02 検証 |
| ~~R-08~~ | ~~マルチスレッド間引き~~ | ~~中~~ | ~~完了~~ | ~~Phase 10-3~~ | ~~std::barrier + worker threads。4ch/8ch×1G 0-drops 達成。decimate_ms 18ms→0.3ms (40-80x)~~ |
| R-09 | 代替描画プリミティブ | 低 | 未着手 | — | Instanced Quad 等 |
| ~~R-10~~ | ~~ネイティブ Vulkan ドライバ検証~~ | ~~高~~ | ~~完了~~ | ~~Phase 7~~ | ~~MSVC + NVIDIA ネイティブで dzn 比 2.6x、L3=2,022 FPS~~ |
| R-11 | Shared Memory IPC への移行検討 | 低 | 延期 | — | TI-08: ボトルネックが transport ではなく消費側 (ring drain + cache cold) のため投資対効果低。Embedded でも同水準 drops |
| ~~R-12~~ | ~~Windows IPC コマンドチャネル実装~~ | ~~中~~ | ~~完了~~ | ~~Phase 9~~ | ~~`PeekNamedPipe` による非ブロッキング実装。IPC `--profile` レート変更が Windows で動作~~ |
| ~~R-13~~ | ~~Release ビルドでの計測標準化~~ | ~~高~~ | ~~完了~~ | ~~Phase 10-3~~ | ~~Debug ビルドで --profile/--bench 実行時に警告表示。リング 16M→64M デフォルト~~ |
