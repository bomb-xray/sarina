# smile

Temporary anonymous compute prototype for a local CPU/CUDA validation run.

## Files

- `smile.cpp` — complete CPU application: event engine, Persian teacher, dashboard and checkpoint
- `smile_cuda.cu` — CUDA validation core for the real normal/memory neuron VM
- `persian_words.tsv` — curated UTF-8 Persian dictionary (readable TSV)
- `my_words.tsv` — personal verified/suggested/blocked words; safe to edit
- `run.ps1` — local Windows build/run script
- `smile.exe` — prebuilt Windows CPU executable in the downloadable CPU package
- `brain.dat` — generated checkpoint

## Persian dictionary and personal suggestions

The base file is human-readable and alphabetically sorted:

```text
word<TAB>frequency<TAB>status<TAB>source_or_note
```

Membership comes from 81,063 POS-tagged entries in the curated Lilak spell-check lexicon; its 12,624 untagged/user entries were excluded. Subtitle frequency is only a statistical weight. The previous raw 155k subtitle-type dump was removed.

Put edits in `my_words.tsv`, not the generated base:

```text
نورومورفیک	10	suggested	نیازمند بازبینی
ایبوپروفین	10	verified	نام دارو
واژهغلط	0	blocked	غلط تایپی
```

- `verified`: trains dictionary and spelling judge
- `suggested`: visible for review but gives no training signal
- `blocked`: removes a base word

The output codec now contains only the 32 Persian letters, `آ`, space, and four common punctuation marks. Arabic hamza forms, digits, and noisy symbols were removed.

## Does the automatic teacher learn?

The old implementation did not: A/B runs produced identical event fingerprints because reward only changed mana credit. Checkpoint-v5 adds persistent causal `plasticity` that changes firing cadence/refractory/gating, so feedback now changes behavior.

It still did **not** win the clean-data A/B. Across three seeds (1000 neurons, 350 virtual seconds), automatic plasticity changed exact-word rate from 5.21% to 4.98% and average quality from 12.09 to 11.43; only the last-10-word quality rose slightly. Therefore automatic strength defaults to **0**: scoring and metrics remain active, but automatic behavioral modification is opt-in and experimental. This is an evaluator, not yet proven language training.

## CUDA test on Windows

Requirements:

1. NVIDIA driver (`nvidia-smi` must work)
2. Visual Studio 2022 Build Tools with **Desktop development with C++**
3. CUDA Toolkit (`nvcc --version` must work)

Toolkit compatibility matters:

- GTX 900 and GTX 10 series (compute capability below 7.5): use **CUDA 12.9**. CUDA 13 removed offline compilation for these GPUs.
- GTX 16 series (Turing, 7.5): CUDA 12.9 or CUDA 13.x.

Open PowerShell in this folder:

```powershell
powershell -ExecutionPolicy Bypass -File .\run.ps1 -Gpu
```

CUDA-test defaults:

```text
neurons       32,000 (fixed real brain, no synthetic duplication)
GPU ceiling   70% (hard maximum; configurable from 10% to 70%)
duration      120 seconds
device        0
```

The first CPU run after this size change archives any existing checkpoint as `brain-before-32k-<date>.dat` and starts a clean 32k brain. Later runs continue the new `brain.dat`. The engine also rejects a loaded checkpoint whose neuron count differs from the requested count.

Measured in the two-logical-CPU sandbox, the current 32k build used about **98 MiB peak RAM** and created a **12.8 MiB checkpoint**. Building the brain, running one virtual second and saving took **3.1 wall seconds**, processing 4.73M events with zero VM faults.

Custom duration or a lower ceiling (10–70%):

```powershell
powershell -ExecutionPolicy Bypass -File .\run.ps1 -Gpu -GpuLimit 70 -Seconds 300
```

The script detects the GPU compute capability, builds the native `sm_XX` target, runs the CUDA core and prints every half second:

- actual NVML GPU utilization and the configured ceiling
- controller duty cycle
- temperature
- virtual-time speed
- firing rate
- signals and dropped signal count

### Important interpretation

`70%` is a **hard ceiling**, not fake target padding. With only 32,000 real neurons, a strong GTX may naturally remain below 70%. The program does not duplicate work just to make Task Manager show a larger number.

The corrected source has been compiled and linked with the real NVIDIA NVCC 13.3 compiler for `sm_75`, and the resulting binary's `--help` path was executed. That proves the CUDA translation unit builds for Turing; it does **not** claim a GPU runtime test. GTX 900/10-series hardware still needs the documented CUDA 12.9 target-machine build and run.

On Windows Task Manager select the GPU graph named **CUDA** or **Compute**, not only `3D`. The console's NVML value is the primary measurement.

A thermal guard reduces duty at 85°C and stops at 90°C.

## What the CUDA validation core really executes

- 1 ms dependency windows
- CUDA Graph launch path when supported
- the same linear bytecode programs for normal neurons
- the same semi-linear bytecode programs and 1 KB local memory for memory neurons
- 20/40 real edges per neuron with 1–20 ms delay
- signal ring, input freshness and output masks
- per-neuron mana, firing cost, upkeep, lobe pools and delayed return
- fixed 14-neuron mouth bottleneck
- normal/memory population on GPU; giant population is counted as host-side work

This is not a matrix-multiplication stress test. It validates the mass-neuron CUDA architecture. The first CUDA run intentionally does **not** replace the complete CPU application yet: teacher scoring, exact causal feedback, dashboard and CPU↔giant signal exchange stay in `smile.cpp` until the hardware test confirms kernel correctness and throughput.

## CPU fallback

The full 32k CPU application is now the default while no strong GPU test machine is available:

```powershell
powershell -ExecutionPolicy Bypass -File .\run.ps1
```

`-Cpu` remains accepted as an explicit alias.

The complete CPU package can use its bundled `smile.exe` without a compiler. If `g++` is installed, the runner rebuilds it from `smile.cpp`. It then continues a matching `brain.dat` when present and opens:

```text
http://localhost:8420
```

Manual build on Linux/macOS:

```bash
g++ -O2 -std=c++17 -pthread smile.cpp -o smile
./smile --neurons 32000 --port 8420 --words persian_words.tsv
```

Manual CUDA build on Linux (add your GPU architecture):

```bash
nvcc -O3 -std=c++17 -arch=sm_75 smile_cuda.cu -o smile-gpu -ldl
./smile-gpu --neurons 32000 --gpu-limit 70 --seconds 120
```

## Test report to keep

After the run, keep or send:

```powershell
nvidia-smi
nvcc --version
```

and the full `smile-gpu.exe` console output. The critical fields are GPU utilization, virtual speed, signal drops, temperature and compute capability.
