# TI-Phase9.1: UDP モード ボトルネック調査

Phase 9 で実装した UDP トランスポートのスループット上限・ボトルネックを定量的に特定する。

## 環境

- **Platform:** WSL2 (Linux 6.6.87.2-microsoft-standard-WSL2)
- **GPU:** RTX 5080 via Mesa dzn (D3D12→Vulkan translation)
- **Build:** GCC, `-O2` (CMake Release), C++20
- **Tool:** `grebe-bench --udp --duration=5`
- **Data:** `bench_20260211_172519.json`
- **既知制約:** WSL2 loopback は UDP データグラム > 1472 byte をドロップする (GitHub #6082)
  - 対策: データグラムサイズを 1400 byte に制限済み
  - `sizeof(FrameHeaderV2)` = 64 byte → ペイロード上限 1336 byte

---

## T-1: UDP ループバック スループット上限

全速送信 (レート制限なし) でのスループット上限を測定。

| シナリオ | ch | block_size | フレーム/秒 | MSPS | MB/s | drop rate |
|---|---|---|---|---|---|---|
| 1ch max | 1 | 668 | 155,278 | 103.7 | 197.8 | 0.78% |
| 1ch 256 | 1 | 256 | 157,884 | 40.4 | 77.1 | 0.50% |
| 1ch 64 | 1 | 64 | 153,764 | 9.8 | 18.8 | 0% |
| 2ch max | 2 | 334 | 157,671 | 52.7 | 200.9 | ~0% |
| 4ch max | 4 | 167 | 153,947 | 25.7 | 196.1 | 0.02% |

**結論:**
- フレームレート上限: **~155K frames/sec** (block_size によらず一定)
- MSPS 上限: 1ch で **~104 MSPS** (block_size=668 の場合)
- データ転送上限: **~200 MB/s** (1400 byte × 155K fps)
- ボトルネック: sendto/recvfrom **システムコールオーバーヘッド** (~6.4 μs/frame)

---

## T-3: フレームサイズ vs スループット特性

| block_size | bytes/frame | frames/sec | MSPS (1ch) | 効率 |
|---|---|---|---|---|
| 64 | 192 | 153,764 | 9.8 | 14.7 MSPS/100Kfps |
| 256 | 576 | 157,884 | 40.4 | 25.6 MSPS/100Kfps |
| 668 | 1,400 | 155,278 | 103.7 | 66.8 MSPS/100Kfps |

**結論:**
- フレームレートはペイロードサイズにほぼ依存しない (~154-158K fps)
- MSPS はフレームサイズに比例 → **フレームを大きくするほど高効率**
- 小フレーム (64 samples) では syscall オーバーヘッドが支配的で MSPS が低い
- MTU 上限 (668 samples) が最も効率的

---

## T-1b: 目標レート制限テスト

レート制限付き送信でのドロップ率を測定。UDP が要求レートに追従できるかを確認。

| 目標レート | 実測 MSPS | frames/sec | drop rate | 判定 |
|---|---|---|---|---|
| 1 MSPS | 1.0 | 1,497 | 0% | ✅ PASS |
| 10 MSPS | 10.0 | 14,970 | 0% | ✅ PASS |
| 100 MSPS | 99.1 | 148,292 | 0% | ✅ PASS |
| 1 GSPS | 97.3 | 145,591 | 1.33% | ⚠️ 上限到達 |

**結論:**
- **1 MSPS ~ 100 MSPS: 0% drops** — UDP トランスポートは安定
- **1 GSPS: 上限到達** — 実効 ~97 MSPS (要求の 9.7%)、drop 1.3%
- Phase 9 受入条件 (1ch × 10 MSPS, 0 drops) は **PASS**
- 100 MSPS も 0 drops で通過 — WSL2 loopback で十分な性能

---

## 分析: ボトルネック要因

### 1. syscall オーバーヘッド (支配的)

```
フレーム間隔 = 1 / 155,000 fps ≈ 6.4 μs
sendto + recvfrom の合計オーバーヘッド ≈ 6.4 μs/frame
```

UDP loopback はカーネル内コピーであり、ネットワーク遅延はゼロ。
ボトルネックは純粋にシステムコール呼び出しのコンテキストスイッチとバッファコピー。

### 2. WSL2 MTU 制限 (構造的)

- WSL2 loopback: 最大 1472 byte/datagram (安全マージン込みで 1400 byte に制限)
- 1ch block_size=668 が上限 → 1 フレームで送れるサンプル数が制約
- Windows native では 65000 byte データグラムが通る可能性あり → T-5 で検証予定

### 3. 理論上限との比較

```
UDP 上限:     104 MSPS (1ch, WSL2)
pipe 上限:    1,000+ MSPS (同一プロセス内、syscall 2回/frame のみ)
理論限界差:   ~10x (pipe vs UDP)
```

UDP は per-frame の syscall コストが pipe より高い (sendto + recvfrom vs write + read)。
ただし 100 MSPS まで 0 drops なので、実用上は十分。

---

## T-5: WSL2 vs Windows native UDP 性能差

Windows native (MSVC Release) で同一ベンチマークを実行し、WSL2 loopback との差を測定。

- **Platform:** Windows native (MSVC 19.44, Ninja, Release)
- **Data:** `bench_windows.json` (2026-02-11T17:30:43)
- **注意:** データグラムサイズは WSL2 と同じ 1400 byte に制限（コード上の定数）

### T-5a: 全速送信 (unlimited rate) — WSL2 vs Windows native

| シナリオ | WSL2 fps | Win fps | WSL2 MSPS | Win MSPS | WSL2 drop | Win drop | fps 倍率 |
|---|---|---|---|---|---|---|---|
| 1ch max (668) | 155,278 | 219,995 | 103.7 | 147.0 | 0.78% | 0% | 1.42x |
| 1ch 256 | 157,884 | 222,299 | 40.4 | 56.9 | 0.50% | 0% | 1.41x |
| 1ch 64 | 153,764 | 233,483 | 9.8 | 14.9 | 0% | 0% | 1.52x |
| 2ch max (334) | 157,671 | 229,345 | 52.7 | 76.6 | ~0% | 0% | 1.45x |
| 4ch max (167) | 153,947 | 189,712 | 25.7 | 31.7 | 0.02% | 0% | 1.23x |

### T-5b: 目標レート制限 — WSL2 vs Windows native

| 目標レート | WSL2 MSPS | Win MSPS | WSL2 drop | Win drop | 判定 |
|---|---|---|---|---|---|
| 1 MSPS | 1.0 | 1.0 | 0% | 0% | ✅ 両方 PASS |
| 10 MSPS | 10.0 | 10.0 | 0% | 0% | ✅ 両方 PASS |
| 100 MSPS | 99.1 | 100.0 | 0% | 0% | ✅ 両方 PASS |
| 1 GSPS | 97.3 | 115.6 | 1.33% | 0% | ⚠️ Win 優位 |

### T-5 分析

**主要な発見:**

1. **Windows native は 1.4x 高速** — 全速送信で ~220K fps (WSL2: ~155K fps)
   - syscall オーバーヘッド: 4.5 μs/frame (WSL2: 6.4 μs/frame)
   - WSL2 の Hyper-V 仮想化オーバーヘッドが ~30% のコスト

2. **Windows native は 0% drops** — 全シナリオ (1 GSPS 含む) で完全無損失
   - WSL2 では全速送信時に 0.5-0.8% drops → Windows native では解消
   - 1 GSPS ターゲットでも drops なし (実効 115.6 MSPS)

3. **MTU 制限はコード定数** — Windows native でもデータグラムは 1400 byte に制限
   - Windows loopback は 65535 byte データグラムを許容するため、
     MTU 制限を解除すれば更なる MSPS 向上が期待できる
   - 例: block_size=32000 (65000 byte payload) なら理論上 220K fps × 32K = 7,040 MSPS
   - ただし UDP_MAX_DATAGRAM のプラットフォーム依存化が必要

4. **1ch 最大スループット: 147 MSPS** (WSL2: 104 MSPS、1.42x 向上)
   - 同一 1400 byte データグラムでの純粋な syscall 効率差

---

## T-5c: Windows native — データグラムサイズ拡大テスト

`--datagram-size=N` オプションで UDP データグラムサイズを 1400 → 65000 byte まで拡大し、
Windows native loopback で大データグラムが通るか、スループットがどの程度向上するかを測定。

- **Tool:** `grebe-bench --udp --duration=5 --datagram-size=N`
- **Data:** `bench_dg{1400,4000,16000,32000,65000}.json`

### T-5c-1: 1ch 全速送信 — データグラムサイズ vs スループット

| datagram (bytes) | block_size | frames/sec | MSPS | MB/s | drop | MSPS 倍率 (vs 1400) |
|---|---|---|---|---|---|---|
| 1,400 | 668 | 227,108 | 151.7 | 280 | 0% | 1.0x |
| 4,000 | 1,968 | 199,098 | 391.8 | 760 | 0% | 2.6x |
| 16,000 | 7,968 | 166,531 | 1,327 | 2,539 | 0% | 8.7x |
| 32,000 | 15,968 | 142,708 | 2,279 | 4,358 | 0% | 15.0x |
| 65,000 | 32,468 | 103,819 | 3,371 | 6,447 | 0% | 22.2x |

### T-5c-2: 1 GSPS ターゲット — データグラムサイズ別到達性

| datagram (bytes) | block_size | 実効 MSPS | frames/sec | drop | 1 GSPS 到達 |
|---|---|---|---|---|---|
| 1,400 | 668 | 117.7 | 176,260 | 0% | ❌ (11.8%) |
| 4,000 | 1,968 | 371.1 | 188,576 | 0% | ❌ (37.1%) |
| 16,000 | 7,968 | 1,000.0 | 125,502 | 0% | ✅ **達成** |
| 32,000 | 15,968 | 1,000.0 | 62,625 | 0% | ✅ 達成 (余裕あり) |
| 65,000 | 32,468 | 1,000.0 | 30,800 | 0% | ✅ 達成 (大幅余裕) |

### T-5c-3: マルチチャネル — 大データグラム時の性能 (全速)

| datagram (bytes) | 2ch MSPS | 2ch fps | 4ch MSPS | 4ch fps |
|---|---|---|---|---|
| 1,400 | 76.6 | 229K | 31.7 | 190K |
| 4,000 | 163.0 | 166K | 103.0 | 209K |
| 16,000 | 725.4 | 182K | 360.6 | 181K |
| 32,000 | 1,003.7 | 126K | 503.2 | 126K |
| 65,000 | 1,507.4 | 93K | 733.4 | 90K |

### T-5c 分析

**主要な発見:**

1. **1 GSPS via UDP 達成** — datagram_size ≥ 16,000 bytes で 1 GSPS (1ch) に到達
   - 16K datagrams: ちょうど 1,000 MSPS (125K fps × 7,968 samples)
   - 32K datagrams: 1,000 MSPS (余裕あり、最大 2,279 MSPS)
   - 65K datagrams: 1,000 MSPS (大幅余裕、最大 3,371 MSPS)

2. **スループットはデータグラムサイズに略比例** — syscall 回数が律速のため
   - 1,400 → 65,000 bytes: block_size 48.6x 増加、MSPS 22.2x 増加
   - フレームレートは低下 (227K → 104K fps) するが、per-frame サンプル数の増加が支配的
   - 大データグラムでは memcpy コストが顕在化するため純粋な比例にはならない

3. **全データグラムサイズで 0% drops** — Windows native loopback は 65K datagrams を完全無損失で処理

4. **WSL2 では不可能** — WSL2 loopback は 1472 byte 超のデータグラムをドロップするため、
   大データグラムによる高速化は Windows native 限定

5. **マルチチャネルでも大幅向上** — 4ch × 65K datagram で 733 MSPS (1400 byte 時の 31.7 MSPS → 23x)

---

## T-5d: 4ch マルチチャネル — Windows native 詳細ベンチマーク

4ch での UDP スループット・ボトルネックをデータグラムサイズ別に定量分析。

- **Tool:** `grebe-bench --udp --channels=4 --duration=5 --datagram-size=N`
- **Data:** `bench_4ch_dg{1400,4000,16000,32000,65000}.json`

### T-5d-1: 4ch 全速送信 — データグラムサイズ vs 1ch あたりスループット

| datagram (bytes) | block_size/ch | frames/sec | MSPS/ch | 合計 MSPS | drop | MSPS 倍率 (vs 1400) |
|---|---|---|---|---|---|---|
| 1,400 | 167 | 207,226 | 34.6 | 138 | 0% | 1.0x |
| 4,000 | 492 | 208,084 | 102.4 | 410 | 0% | 3.0x |
| 16,000 | 1,992 | 178,474 | 355.5 | 1,422 | 0% | 10.3x |
| 32,000 | 3,992 | 142,652 | 569.5 | 2,278 | 0% | 16.5x |
| 65,000 | 8,117 | 104,754 | 850.3 | 3,401 | 0.02% | 24.6x |

### T-5d-2: 4ch 目標レート — データグラムサイズ別到達性

| datagram (bytes) | 10 MSPS/ch | 100 MSPS/ch | 1 GSPS/ch | 100 MSPS 判定 |
|---|---|---|---|---|
| 1,400 | ✅ 10.0 | ❌ 29.6 (上限) | ❌ 29.5 | FAIL |
| 4,000 | ✅ 10.0 | ✅ 99.6 | ❌ 103.5 (上限) | PASS |
| 16,000 | ✅ 10.0 | ✅ 100.0 | ❌ 294.4 (29%) | PASS |
| 32,000 | ✅ 10.0 | ✅ 100.0 | ❌ 483.8 (48%) | PASS |
| 65,000 | ✅ 10.0 | ✅ 100.0 | ❌ 754.3 (75%) | PASS |

### T-5d-3: 1ch vs 4ch 比較 (同一データグラムサイズ)

| datagram (bytes) | 1ch MSPS | 4ch MSPS/ch | 4ch 合計 MSPS | 効率 (合計/1ch) |
|---|---|---|---|---|
| 1,400 | 152 | 34.6 | 138 | 91% |
| 4,000 | 392 | 102.4 | 410 | 105% |
| 16,000 | 1,327 | 355.5 | 1,422 | 107% |
| 32,000 | 2,279 | 569.5 | 2,278 | 100% |
| 65,000 | 3,371 | 850.3 | 3,401 | 101% |

### T-5d ボトルネック分析

**構造的ボトルネック: チャネルはデータグラムペイロードを共有**

```
1 フレームのペイロード = channels × block_size × sizeof(int16_t)
block_size/ch = (datagram_size - 64) / (channels × 2)

4ch の block_size/ch は 1ch の 1/4:
  1ch @ 65000B: block_size = 32,468 samples
  4ch @ 65000B: block_size =  8,117 samples/ch (1/4)
```

**定量分析:**

1. **合計スループットはチャネル数に依存しない** (~3,400 MSPS @ 65000B)
   - 1ch: 3,371 MSPS, 4ch 合計: 3,401 MSPS → 効率 101%
   - ボトルネックは syscall + memcpy であり、チャネル数でなくペイロードサイズで決定
   - チャネルを増やしても合計転送量は変わらず、1ch あたりの帯域が等分される

2. **4ch で 100 MSPS/ch (400 MSPS 合計) には datagram_size ≥ 4,000B が必要**
   - 1400B: フレームレート上限 ~207K fps × 167 samples/ch = 34.6 MSPS/ch → 100 MSPS に届かない
   - 4000B: フレームレート ~208K fps × 492 samples/ch = 102.4 MSPS/ch → ちょうど到達

3. **4ch で 1 GSPS/ch (4 GSPS 合計) は UDP では不可能**
   - 65000B でも 850 MSPS/ch (75%) が上限
   - 必要帯域: 4 GSPS × 2 bytes = 8 GB/s → syscall ベースの UDP では非現実的
   - 達成には共有メモリ + ゼロコピーが必要

4. **フレームレートの一貫性**
   - 全データグラムサイズで ~105K-210K fps (1ch と同等)
   - チャネル数を増やしても syscall オーバーヘッドは変わらない
   - per-channel MSPS = (フレームレート × block_size/ch) で決定

**実用的な推奨設定 (4ch):**

| 目標 | 推奨 datagram_size | 必要フレームレート |
|---|---|---|
| 10 MSPS/ch | 1,400 B (デフォルト) | ~60K fps |
| 100 MSPS/ch | ≥ 4,000 B | ~200K fps |
| 250 MSPS/ch (1 GSPS 合計) | ≥ 16,000 B | ~126K fps |
| 500 MSPS/ch (2 GSPS 合計) | ≥ 32,000 B | ~125K fps |

---

## 未実施項目

| ID | 項目 | 状態 |
|---|---|---|
| T-2 | pipe vs UDP E2E 比較 (viewer FPS) | 手動テスト待ち |
| T-4 | E2E レイテンシ (UDP モード) | 手動テスト待ち |

---

## まとめ

### 1ch

| 指標 | WSL2 (1400B) | Win native (1400B) | Win native (65000B) | 評価 |
|---|---|---|---|---|
| フレームレート上限 | ~155K fps | ~220K fps | ~104K fps | 大 DG は fps 低下 |
| 1ch スループット上限 | 104 MSPS | 147 MSPS | **3,371 MSPS** | 大 DG で 32x |
| 1 GSPS 到達性 | 97 MSPS ❌ | 116 MSPS ❌ | **1,000 MSPS ✅** | **16K DG で達成** |

### 4ch (Windows native)

| 指標 | 1400B | 4000B | 16000B | 65000B |
|---|---|---|---|---|
| フレームレート上限 | 207K fps | 208K fps | 178K fps | 105K fps |
| MSPS/ch 上限 | 35 | 102 | 356 | **850** |
| 合計 MSPS 上限 | 138 | 410 | 1,422 | **3,401** |
| 100 MSPS/ch | ❌ | ✅ | ✅ | ✅ |
| 1 GSPS/ch | ❌ | ❌ | ❌ | ❌ (75%) |

### 共通

| 指標 | 値 | 評価 |
|---|---|---|
| 10 MSPS 安定性 (全構成) | 0% drops | ✅ Phase 9 受入条件 PASS |
| 100 MSPS 安定性 (全構成) | 0% drops | ✅ PASS (4ch は ≥4000B 必要) |
| ボトルネック | sendto/recvfrom syscall | チャネル数に依存せず合計帯域一定 |
| 合計スループット上限 | ~3,400 MSPS @ 65000B | 1ch/4ch 同等 |
