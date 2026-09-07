
### Text and structured data

| File | What it is | Size | Mine | gzip -6 | Diff | Mine MB/s | gzip MB/s | Mine 8t MB/s | Decomp MB/s |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `access.log` | Server access log (generated) | 12,655,301 | 88.07% | 87.68% | +0.39 | 29.7 | 71.2 | 105.6 | 159.4 |
| `data.json` | JSON API records (generated) | 18,088,061 | 91.18% | 91.20% | -0.02 | 50.2 | 134.4 | 175.3 | 242.5 |
| `data.csv` | CSV export (generated) | 12,790,937 | 73.92% | 74.29% | -0.37 | 14.9 | 28.8 | 46.6 | 117.8 |
| `xml` | XML documents (Silesia) | 5,345,280 | 87.11% | 87.05% | +0.06 | 31.7 | 64.3 | 73.4 | 49.0 |
| `nci` | Chemical database (Silesia) | 33,553,445 | 90.61% | 90.46% | +0.15 | 39.0 | 89.6 | 131.9 | 223.8 |
| `dickens` | English prose (Silesia) | 10,192,446 | 61.91% | 62.04% | -0.14 | 9.9 | 16.8 | 30.4 | 73.0 |
| `webster` | Dictionary text (Silesia) | 41,458,703 | 70.56% | 70.57% | -0.01 | 17.1 | 30.5 | 67.2 | 36.8 |
| `reymont` | Polish prose, PDF (Silesia) | 6,627,202 | 71.82% | 71.95% | -0.13 | 9.6 | 19.4 | 34.6 | 105.0 |
| `samba` | Source tarball (Silesia) | 21,606,400 | 74.23% | 74.73% | -0.49 | 27.0 | 46.9 | 86.5 | 136.9 |
| `source.txt` | C++ source, this project | 92,330 | 71.33% | 71.54% | -0.21 | 8.7 | 36.3 | 9.6 | 12.3 |
| **total** | | **162,410,105** | **79.17%** | **79.22%** | **-0.05** | | | | |

### Binary and scientific data

| File | What it is | Size | Mine | gzip -6 | Diff | Mine MB/s | gzip MB/s | Mine 8t MB/s | Decomp MB/s |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `osdb` | Database dump (Silesia) | 10,085,684 | 63.23% | 62.92% | +0.30 | 28.3 | 35.6 | 84.1 | 105.1 |
| `mozilla` | Binary tarball (Silesia) | 51,220,480 | 62.06% | 62.81% | -0.75 | 22.2 | 27.7 | 81.6 | 103.1 |
| `ooffice` | Shared library (Silesia) | 6,152,192 | 49.56% | 49.66% | -0.10 | 16.7 | 21.6 | 57.4 | 53.2 |
| `shell32.dll` | Windows DLL | 7,947,424 | 53.86% | 54.19% | -0.33 | 16.5 | 21.2 | 61.9 | 54.3 |
| `sao` | Star catalogue, floats (Silesia) | 7,251,944 | 26.28% | 26.46% | -0.18 | 12.7 | 18.7 | 48.1 | 60.3 |
| `mr` | Medical MRI scan (Silesia) | 9,970,564 | 62.60% | 62.99% | -0.39 | 12.1 | 19.7 | 37.6 | 104.0 |
| `x-ray` | Medical X-ray image (Silesia) | 8,474,240 | 29.64% | 28.75% | +0.89 | 22.8 | 26.7 | 73.7 | 68.2 |
| **total** | | **101,102,528** | **55.54%** | **55.90%** | **-0.36** | | | | |

### Already compressed

| File | What it is | Size | Mine | gzip -6 | Diff | Mine MB/s | gzip MB/s | Mine 8t MB/s | Decomp MB/s |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `photo.jpg` | JPEG image | 298,161 | 4.57% | 4.46% | +0.10 | 19.8 | 27.9 | 20.4 | 32.6 |
| `image.png` | PNG image | 880,926 | 0.15% | 0.10% | +0.04 | 26.7 | 40.7 | 26.0 | 49.1 |
| `audio.mp3` | MP3 audio | 2,161,197 | 0.89% | 0.80% | +0.09 | 27.9 | 37.5 | 49.1 | 54.6 |
| `video.mp4` | H.264 video | 10,837,627 | 0.03% | 0.02% | +0.01 | 39.5 | 42.3 | 102.5 | 491.2 |
| `archive.zip` | ZIP archive | 68,182,744 | 0.29% | 0.33% | -0.04 | 37.9 | 38.7 | 116.7 | 147.8 |
| `random.bin` | Uniform random bytes | 3,000,000 | -0.00% | -0.02% | +0.01 | 37.2 | 41.2 | 68.2 | 271.1 |
| **total** | | **85,360,655** | **0.28%** | **0.30%** | **-0.02** | | | | |

**Whole corpus:** 348,873,288 bytes. Mine 53.02%, gzip -6 53.15%, difference -0.13 points.

Roundtrip verified byte-identical on 23/23 files.
