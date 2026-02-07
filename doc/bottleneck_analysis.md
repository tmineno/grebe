# Grebe — ボトルネック分析レポート

**日付:** 2026-02-07
**環境:** WSL2 (Linux 6.6.87), llvmpipe (software Vulkan), AMD Ryzen 9 9950X3D (16C/32T)

---

## 1. 性能目標達成状況

| レベル | 入力レート | 間引き | 目標FPS | 結果FPS | 判定 |
|---|---|---|---|---|---|
| L0 | 1 MSPS | MinMax | 60 | 60.0 | **PASS** |
| L1 | 100 MSPS | MinMax | 60 | 59.9 | **PASS** |
| L2 | 1 GSPS | MinMax | 60 | 59.7 | **PASS** |
| L3 | 1 GSPS (V-Sync OFF) | MinMax | 計測のみ | — | 未計測 (llvmpipe制約) |

全シナリオで 60 FPS を達成。L3 は llvmpipe 環境では FIFO プレゼントモード固定のため計測不可。

## 2. パイプライン各段の所要時間

データソース: `tmp/profile_20260207_193059.json` (300フレーム計測、120フレームウォームアップ)

### フレーム時間内訳 (平均値, ms)

| ステージ | 1 MSPS | 10 MSPS | 100 MSPS | 1 GSPS | スケーリング |
|---|---|---|---|---|---|
| drain_ms | 0.001 | 0.000 | 0.000 | 0.000 | 一定 |
| decimate_ms | 0.053 | 0.050 | 0.111 | 0.214 | ≈入力量に比例 |
| upload_ms | 0.039 | 0.038 | 0.043 | 0.044 | ほぼ一定 (3840 vtx固定) |
| swap_ms | 0.001 | 0.001 | 0.001 | 0.001 | 一定 |
| render_ms | 16.55 | 16.55 | 16.57 | 16.63 | ほぼ一定 (V-Sync律速) |
| **frame_ms** | **16.66** | **16.66** | **16.68** | **16.75** | V-Sync律速 |

### 所見

- **render_ms が全体の 99% を占める**: V-Sync ON (FIFO) のため描画待ち ≈ 16.67ms に張り付く。
- **decimate_ms は 1 GSPS でも 0.21ms**: フレーム時間の約 1.3%。MinMax 処理に十分な余裕あり。
- **upload_ms は入力レートに依存しない**: 間引き後の頂点数 (3840) が一定のため。
- **drain_ms は事実上ゼロ**: ダブルバッファ swap のみで ring buffer からの pop は別スレッドで完了済み。

## 3. 各段の律速要因分析

### 3.1 データ生成

| 方式 | スループット | 備考 |
|---|---|---|
| `std::sin()` 直接計算 | ~20 MSPS | Phase 0 実装、1 MSPS 用 |
| LUT (4096エントリ) | ~400 MSPS | Phase 3 初期実装、FP64 依存チェーンが律速 |
| Period tiling (memcpy) | >1 GSPS | Phase 3 最終実装、memcpy帯域律速 |

**結論**: Period tiling により生成は律速ではなくなった。1 GSPS で実効データレート 1000.0 MSPS を達成。

### 3.2 Ring Buffer

- SPSC lock-free 設計、memcpy ベースの bulk push/pop
- 64M サンプル (128 MB) 設定時、1 GSPS での ring_fill: avg 0.02%, max 0.3%
- **結論**: 十分なマージン。律速ではない。

### 3.3 間引き (Decimation)

| アルゴリズム | スループット (BM-B) | 1 GSPS 余裕 |
|---|---|---|
| MinMax (scalar) | 1,354 MSPS | 1.35x |
| MinMax (SSE2 SIMD) | 1,526 MSPS | 1.53x |
| LTTB | 213 MSPS | **0.21x (不足)** |

**結論**: MinMax SIMD は 1 GSPS の 1.5 倍のスループットがあり、十分。LTTB は 100 MSPS 以上では実用不可。

### 3.4 GPU アップロード

- 間引き後 3840 頂点 × 2 bytes = 7.68 KB/フレーム
- upload_ms ≈ 0.04ms で一定
- **結論**: 全く律速ではない。間引きによりデータ量が 1/10,000 以下に削減されている。

### 3.5 描画 (Render)

| 頂点数 | FPS (BM-C, V-Sync OFF) | frame_ms |
|---|---|---|
| 3,840 | 470 | 2.1 ms |
| 38,400 | 289 | 3.5 ms |
| 384,000 | 101 | 9.9 ms |

**結論**: 3840 頂点では描画コスト 2.1ms。V-Sync の 16.67ms に対して 8 倍の余裕。描画は律速ではない。

## 4. ボトルネックマップ

```
1 MSPS:   生成(余裕) → Ring(余裕) → 間引き(余裕) → Upload(余裕) → 描画(余裕) → [V-Sync 律速]
100 MSPS: 生成(余裕) → Ring(余裕) → 間引き(余裕) → Upload(余裕) → 描画(余裕) → [V-Sync 律速]
1 GSPS:   生成(余裕) → Ring(余裕) → 間引き(1.5x) → Upload(余裕) → 描画(余裕) → [V-Sync 律速]
```

**全レートで V-Sync が律速**。パイプライン自体にボトルネックはない。

1 GSPS で最も余裕が小さいのは **間引き (MinMax SIMD, 1.5x)** であり、入力レートをさらに上げた場合に最初に飽和するステージとなる。理論上の限界は ≈1.5 GSPS (MinMax SIMD 律速)。

## 5. 主要な技術的知見

1. **Period tiling は 1 GSPS 生成の鍵**: per-sample 計算を memcpy に置換することで、CPU の FP64 ループ依存チェーン (≈400 MSPS 上限) を完全に回避。
2. **MinMax SIMD の効果は llvmpipe 上で限定的**: scalar 1354 vs SIMD 1526 MSPS (1.13x)。llvmpipe の LLVM バックエンドが scalar コードを自動ベクトル化している可能性あり。実 GPU 環境では差が拡大する可能性。
3. **GPU Compute 間引きは CPU に劣る** (llvmpipe上): Compute 472 MSPS vs CPU SIMD 1526 MSPS (3.2x 差)。ソフトウェアレンダラのオーバーヘッドが大きい。
4. **LTTB は高レートに不適**: 213 MSPS ではデータ到着に追いつけない。MinMax 専用が現実的。
5. **間引き後のデータ量が一定** (3840 vtx) のため、upload/draw コストはレート非依存。パイプライン設計の正しさが確認された。
