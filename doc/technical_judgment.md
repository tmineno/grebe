# Grebe — 技術的検証項目 回答メモ

**日付:** 2026-02-07
**環境:** WSL2 (Linux 6.6.87), llvmpipe (software Vulkan), AMD Ryzen 9 9950X3D (16C/32T)

---

## TI-01: GPU 転送ボトルネック

> PCIe 帯域に対して Vulkan vkCmdCopyBuffer の実効スループットはどこまで出るか。Staging Buffer のサイズと転送頻度のトレードオフ。

### 計測結果 (BM-A)

| バッファサイズ | スループット | 備考 |
|---|---|---|
| 1 MB | 11.4 GB/s | オーバーヘッド比率大 |
| 4 MB | 31.6 GB/s | ほぼ飽和 |
| 16 MB | 35.9 GB/s | ピーク |
| 64 MB | 17.4 GB/s | キャッシュミス/TLB ミスで低下 |

### 回答

**注意**: llvmpipe 環境では vkCmdCopyBuffer は CPU メモリ間コピー (memcpy) として実行されるため、計測値は PCIe 帯域ではなく **メモリ帯域** を反映している。実 GPU 環境では PCIe Gen3 x16 (理論 ~16 GB/s)、Gen4 (理論 ~32 GB/s) が上限となる。

**Staging バッファ戦略**: 間引き後の転送データは 3840 vtx × 2 bytes = 7.68 KB/フレーム。この微小サイズでは PCIe 帯域は全く問題にならない。**CPU 側間引きにより転送量を 1/10,000 以下に削減する設計は正しい。**

**結論**: GPU 転送はボトルネックではない。実ハードウェア検証を推奨。

---

## TI-02: CPU 間引き性能

> MinMax の SIMD 最適化でどこまで出るか。LTTB は 1 GSPS に対して実用的か。マルチスレッド並列間引きの効果。

### 計測結果 (BM-B)

| アルゴリズム | スループット | 1 GSPS 余裕 |
|---|---|---|
| MinMax (scalar) | 1,354 MSPS | 1.35x |
| MinMax (SSE2 SIMD) | 1,526 MSPS | 1.53x |
| LTTB | 213 MSPS | 0.21x (**不足**) |

### 回答

- **MinMax SIMD は 1 GSPS を処理可能** (1.5x マージン)。AVX2 化でさらなる向上が見込める。
- **LTTB は 100 MSPS 以上で実用不可**。O(n) ではあるが分岐が多く、SIMD 化が困難。高レートでは MinMax 一択。
- **SIMD の効果が限定的** (1.13x): llvmpipe の LLVM バックエンドが scalar コードを自動ベクトル化している可能性。実 CPU + 実 GPU 環境では手動 SIMD の効果が拡大する可能性がある。
- **マルチスレッド並列間引き**: 本 PoC では未実装。1 GSPS で 1.5x マージンがあるため、現時点では不要。2 GSPS 以上を目指す場合に検討。

**結論**: MinMax SIMD で 1 GSPS は達成可能。LTTB は高レートに不適。

---

## TI-03: GPU 側間引きの可能性

> Compute Shader で間引きを行った場合の性能差。CPU 間引き vs GPU 間引きどちらが速いか。

### 計測結果 (BM-E)

| 方式 | スループット | 備考 |
|---|---|---|
| CPU MinMax (SIMD) | 1,526 MSPS | SSE2, 16M samples → 3840 vtx |
| GPU Compute MinMax | 472 MSPS | 256 threads/workgroup, shared memory reduction |

### 回答

**CPU が 3.2 倍高速** (llvmpipe 環境)。

理由:
1. llvmpipe はソフトウェアレンダラであり、compute shader のディスパッチオーバーヘッドが大きい
2. CPU→GPU→CPU のデータ往復コスト (int16→int32 変換 + readback) が支配的
3. 実 GPU 環境では GPU compute が逆転する可能性あり (特に PCIe Direct Storage/ReBAR 環境)

**重要な設計上の知見**: CPU 側で間引いた結果 (3840 vtx = 7.68 KB) だけを GPU に送る方が、raw データ (16M samples = 32 MB) を GPU に送って GPU 側で間引くより**転送量が 4,000 倍少ない**。PCIe が律速になる環境では CPU 間引きが圧倒的に有利。

**結論**: CPU 側間引き + 小データ転送が現行最適解。GPU compute は実ハードウェアで再検証を推奨。

---

## TI-04: 描画プリミティブ選択

> LINE_STRIP vs 他のプリミティブの描画コスト。頂点数と描画パフォーマンスの関係。

### 計測結果 (BM-C, V-Sync OFF)

| 頂点数 | FPS | frame_ms | 備考 |
|---|---|---|---|
| 3,840 | 470 | 2.1 ms | MinMax 間引き後の標準頂点数 |
| 38,400 | 289 | 3.5 ms | 10x |
| 384,000 | 101 | 9.9 ms | 100x |

### 回答

- **LINE_STRIP は十分高速**: 3840 頂点で 470 FPS (2.1ms/frame)。V-Sync 16.67ms に対して 8 倍の余裕。
- **描画コストのスケーリング**: ほぼ線形 (3.8K→384K で約 5x のコスト増)。
- **律速となる頂点数の目安**: ≈800K 頂点で 16.67ms に到達すると推定 (V-Sync 60 FPS 限界)。
- **他プリミティブ未検証**: Instanced Quad (太線) や GL_POINTS は未実装。MinMax 後の 3840 頂点では LINE_STRIP の性能で十分であり、追加検証の優先度は低い。

**結論**: LINE_STRIP で十分。間引きにより頂点数が一定 (3840) なため、描画は律速にならない。

---

## TI-05: 永続マップドバッファ

> ReBAR/SAM 対応環境での HOST_VISIBLE + DEVICE_LOCAL 直接書き込みは Staging 経由より速いか。

### 回答

**未計測**。llvmpipe 環境では ReBAR/SAM 機能が利用できないため、本 PoC では検証不可。

**設計上の考察**:
- 現行の staging 経由転送は 3840 vtx × 2 bytes = 7.68 KB/フレームと極めて小さい
- この転送量では staging vs 永続マップのどちらでも差は無視できるレベル
- ReBAR の効果が出るのは大データ転送時 (数十 MB 以上)。間引き後の転送には適用理由が薄い
- raw データを GPU に直接転送して GPU 側で間引く構成 (TI-03 関連) では ReBAR の効果が大きい可能性あり

**結論**: 現行設計では優先度低。GPU 側間引き構成を採用する場合に再検討。

---

## TI-06: スレッドモデル

> 生成・間引き・転送・描画の分離粒度の最適解。lock-free ring buffer vs mutex-guarded double buffer。

### 計測結果 (Profile データ)

| 指標 | 1 GSPS 計測値 | 備考 |
|---|---|---|
| ring_fill avg | 0.02% | ほぼ空 — 消費が生成に追従 |
| ring_fill max | 0.3% | 瞬間的な spike のみ |
| drain_ms | 0.0005 ms | ダブルバッファ swap のみ |
| decimate_ms | 0.21 ms | 間引きスレッドでの処理 |

### 回答

**現行の 3 スレッドモデルが有効**:
1. **生成スレッド**: period tiling (memcpy) で 1 GSPS を維持。busy-wait pacing で正確なレート制御。
2. **間引きスレッド**: ring buffer drain + MinMax + ダブルバッファ出力。フレームごとの処理時間 0.21ms。
3. **メインスレッド (描画)**: upload + render + HUD。V-Sync 待ちが大半。

**lock-free SPSC ring buffer の有効性**:
- ring_fill が常時 <1% であり、生産者・消費者間にコンテンションなし
- mutex-guarded 方式と比較して、lock-free は高レートでのレイテンシ変動を抑制
- 1 GSPS でのスループット維持に貢献

**改善の余地**:
- 4 スレッド化 (生成/間引き/転送/描画) は現状不要 — upload_ms が 0.04ms と微小
- 間引きスレッドの sleep (100μs) をイベント駆動に変更すれば latency を削減可能

**結論**: 現行 3 スレッドモデル + lock-free SPSC が最適。4 スレッド化の必要性なし。

---

## 製品化に向けた推奨事項

### 優先度高

1. **実 GPU 環境での再計測**: llvmpipe の結果は参考値。PCIe 帯域、GPU draw コスト、compute shader 性能は大幅に異なる可能性。NVIDIA/AMD 離散 GPU での計測が必須。
2. **マルチチャンネル対応**: 現行 1ch → 4ch/8ch へ。間引き・描画の並列化が必要。
3. **LTTB の高レート無効化**: UI 上で MinMax/LTTB を切替可能だが、100 MSPS 以上では MinMax のみ許可すべき。

### 優先度中

4. **AVX2 MinMax 最適化**: SSE2 → AVX2 で int16 の処理幅が 8→16 に倍増。理論 2x の向上。
5. **ReBAR/SAM 永続マップド検証**: 実 GPU で TI-05 を回答。
6. **GPU Compute 間引きの実 GPU 再検証**: llvmpipe での劣勢が実 GPU で逆転する可能性。

### 優先度低

7. **E2E レイテンシ計測**: NFR-02 の 50ms/100ms 目標の検証。現状は ring buffer + ダブルバッファ + V-Sync の 3 フレーム分 (≈50ms) と推定。
8. **マルチスレッド間引き**: 1 GSPS で 1.5x マージンがあるため、2 GSPS+ を目指す場合のみ。
9. **代替描画プリミティブ**: 太線描画 (Instanced Quad) は視覚品質向上のため、性能検証後に検討。
