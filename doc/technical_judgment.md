# Grebe — 技術的検証項目 (Technical Judgment Log)

本文書はプロジェクトを通じて蓄積される技術的検証の履歴を管理する。各検証項目 (TI) に対し、計測・分析を行うたびにエントリを追記する。

---

## 変更履歴

| 日付 | Phase | 変更内容 |
|---|---|---|
| 2026-02-07 | Phase 3-4 | TI-01〜06 初回回答。llvmpipe (WSL2) 環境での計測結果に基づく |
| 2026-02-07 | Phase 6 | TI-01/03/05 実GPU回答追記。RTX 5080 (dzn) + Release ビルドでの計測。R-01完了 |
| 2026-02-07 | Phase 7 | TI-01/02/03/04 ネイティブ Vulkan 回答追記。MSVC ビルド + NVIDIA ネイティブドライバでの計測。R-10追加 |

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

## 推奨事項トラッカー

`doc/technical_judgment.md` で提起された推奨事項の対応状況を追跡する。

| # | 推奨事項 | 優先度 | ステータス | 対応Phase | 備考 |
|---|---|---|---|---|---|
| R-01 | 実 GPU 環境での再計測 | 高 | 完了 | Phase 6-7 | dzn (Phase 6) + ネイティブ Vulkan (Phase 7) で全ベンチマーク計測済み |
| R-02 | マルチチャンネル対応 (4ch/8ch) | 高 | 完了 | Phase 5 | FR-07 実装済み (fb21264, 387c6fd) |
| R-03 | LTTB の高レート無効化 | 高 | 完了 | Phase 5 | FR-02.6 実装済み (effective_mode_ パターン) |
| R-04 | AVX2 MinMax 最適化 | 中 | 未着手 | — | Release ビルドで 21x マージン → 優先度低下 |
| R-05 | ReBAR/SAM 永続マップド検証 | 低 | 見送り | — | dzn 非対応 + 現設計で恩恵皆無 (TI-05 参照) |
| R-06 | GPU Compute 間引きの実GPU再検証 | 中 | 完了 | Phase 6 | CPU SIMD 7.1x 高速 → CPU 間引き最適を再確認 (TI-03) |
| R-07 | E2E レイテンシ計測 | 低 | 未着手 | — | NFR-02 検証 |
| R-08 | マルチスレッド間引き | 低 | 未着手 | — | Release で 21x マージン → 必要性さらに低下 |
| R-09 | 代替描画プリミティブ | 低 | 未着手 | — | Instanced Quad 等 |
| R-10 | ネイティブ Vulkan ドライバ検証 | 高 | 完了 | Phase 7 | MSVC + NVIDIA ネイティブで dzn 比 2.6x、L3=2,022 FPS |
