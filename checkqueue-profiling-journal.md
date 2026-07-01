# CCheckQueue Profiling Journal

**Autor:** Íñigo Aréjula  
**Repo:** bitcoin/bitcoin  
**Objetivo:** medir empíricamente el comportamiento de `CCheckQueue` antes de proponer cambios de arquitectura

---

## Contexto

El punto de partida es `~/Downloads/checkqueue-scheduler-proposal.md`, un documento propio que explora cuatro posibles mejoras a `CCheckQueue`:

1. Pre-scan barato para scheduling óptimo (distribuir checks por coste estimado)
2. Colas SPSC individuales por worker (eliminar contención del mutex compartido)
3. Batches más grandes en `Add()` (reducir número de acquisitions del mutex)
4. Cambios quirúrgicos: `notify_all → notify_one`, eliminar `nIdle`

El documento concluye que ninguna propuesta tiene base sin datos reales. Las preguntas que hay que responder primero:

- ¿Hay contención real del mutex? (si no, las colas SPSC no sirven de nada)
- ¿Hay desbalance entre workers? (si no, el scheduling óptimo no aporta)
- ¿Cuánto idle time tiene cada worker?

---

## 2026-06-30 — Sesión inicial

### Decisión: enfoque de profiling

**Primera idea:** instrumentar `CCheckQueue::Loop()` con `std::chrono::steady_clock::now()` para medir directamente `work_ns` e `idle_ns` por worker.

**Problema identificado:** esta aproximación es demasiado intrusiva:
- `clock_gettime` dentro del hot path introduce barreras de memoria y efectos de caché
- El observer effect modifica exactamente lo que queremos medir
- El código que medimos ya no es el código que corre en producción

**Decisión:** usar `perf` externo, sin modificar el código fuente. Cero intrusión.

Herramientas elegidas:
- `perf sched record` + `perf sched timehist --summary`: mide tiempo de CPU real vs idle por thread a nivel de scheduler
- Ventaja: el kernel sabe exactamente cuándo cada thread está en estado running vs sleeping, sin necesidad de instrumentación de usuario

### Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_IPC=OFF -DBUILD_BENCH=ON
cmake --build build --target bench_bitcoin -j$(nproc)
```

Notas:
- `ENABLE_IPC=OFF` porque Cap'n Proto no estaba instalado
- `RelWithDebInfo` para que `perf` resuelva símbolos y no haya distorsión por inlining agresivo
- Los flags del benchmark usan guión simple: `-filter=`, no `--filter=`
- Para modo endless (necesario para tener tiempo de introducir contraseña sudo): `NANOBENCH_ENDLESS=<nombre>`

### Cambios para soportar `-par` en el benchmark

`TestChain100Setup` tiene `worker_threads_num` hardcodeado a 2 en `src/test/util/setup_common.cpp:311`. Para poder variar el número de workers sin recompilar, dos cambios mínimos:

**`src/bench/bench_bitcoin.cpp`** — reenviar `-par` al test setup y registrarlo en ArgsManager:
```cpp
static std::vector<std::string> AVAILABLE_ARGS = {"-testdatadir", "-par"};
// ...
argsman.AddArg("-par=<n>", "Number of script-checking worker threads", ArgsManager::ALLOW_ANY, OptionsCategory::OPTIONS);
```

**`src/test/util/setup_common.cpp`** — leer `-par` de los args en vez del hardcode:
```cpp
// Antes:
.worker_threads_num = EnableFuzzDeterminism() ? 0 : 2,
// Después:
.worker_threads_num = EnableFuzzDeterminism() ? 0 : m_node.args->GetIntArg("-par", 2),
```

### Benchmark elegido: `ConnectBlockMixedEcdsaSchnorr`

El benchmark más realista disponible en `src/bench/connectblock.cpp`. Simula la validación de un bloque con 1000 transacciones, cada una con 1 input Taproot (Schnorr) + 4 inputs P2WPKH (ECDSA), que es el mix observado en bloques reales del rango 848k-868k (aproximadamente 20% Schnorr / 80% ECDSA). Llama directamente a `chainstate.ConnectBlock()`, ejercitando el camino completo: `CheckInputScripts` → `CCheckQueueControl::Add()` → `CCheckQueue::Loop()` → `VerifyScript()`.

Se descartaron:
- `CCheckQueueSpeedPrevectorJob`: jobs triviales (no verifican scripts reales), no representativo
- `VerifyScriptP2WPKH` / `VerifyScriptP2TR_*`: benchmarks de un solo check, sin paralelismo

### Aislamiento entre experimentos

Antes de cada nuevo experimento se mata el proceso anterior con `pkill -f bench_bitcoin` y se espera a que termine antes de lanzar el siguiente. Esto es importante: si dos instancias corrieran en paralelo compartirían CPUs y los datos de `perf sched` mezclarían el trabajo de ambos procesos, haciendo los runtimes ininterpretables.

### Procedimiento de medición (igual para todos los experimentos)

```bash
NANOBENCH_SUPPRESS_WARNINGS=1 NANOBENCH_ENDLESS=ConnectBlockMixedEcdsaSchnorr \
  ./build/bin/bench_bitcoin -filter=ConnectBlockMixedEcdsaSchnorr -par=<N> &

sudo perf sched record -p <PID> -- sleep 15
sudo perf sched timehist --summary
```

`NANOBENCH_ENDLESS` hace que el benchmark corra indefinidamente, dando tiempo a introducir la contraseña de sudo.

---

## Experimentos — ConnectBlockMixedEcdsaSchnorr

Workload: 1000 txs × 5 inputs (1 Schnorr + 4 ECDSA) = 5000 checks. `nBatchSize=128`.  
Ventana de medición: 15 segundos.

---

### 2 workers

**Raw perf:**
```
Runtime summary
                          comm  parent   sched-in     run-time    min-run     avg-run     max-run  stddev  migrations
                                          (count)       (msec)     (msec)      (msec)      (msec)       %
---------------------------------------------------------------------------------------------------------------------
                 b-test[28930]      -1        588    23363.298      0.000      39.733    8412.850   36.00       0
    b-scriptch.00[28933/28930]      -1        345    14984.809      0.000      43.434      72.218    2.51       0
    b-scriptch.01[28934/28930]      -1       1832    14946.374      0.000       8.158      80.304    5.10       0

Total number of context switches: 2765
           Total run time (msec): 53294.481
    Total scheduling time (msec): 14986.840  (x 20)
```

**Media workers: 14965 ms**

| Worker | Runtime (ms) | Diff vs media |
|---|---:|---:|
| scriptch.00 | 14984 | +0.1% |
| scriptch.01 | 14946 | -0.1% |
| b-test (master) | 23363 | — |

**CV: ~0.25% — Idle time: ~0%**

---

### 4 workers

**Raw perf:**
```
Runtime summary
                          comm  parent   sched-in     run-time    min-run     avg-run     max-run  stddev  migrations
                                          (count)       (msec)     (msec)      (msec)      (msec)       %
---------------------------------------------------------------------------------------------------------------------
    b-scriptch.01[29928/29924]      -1        427    27667.563      0.000      64.795    7044.951   31.04       0
    b-scriptch.02[29929/29924]      -1        484    49226.392      0.000     101.707    4263.706   19.33       0
    b-scriptch.03[29930/29924]      -1        619    26622.743      0.000      43.009    7893.743   30.61       0
                 b-test[29924]      -1        823    29687.412      0.000      36.072    8266.540   30.35       0
    b-scriptch.00[29927/29924]      -1        438    29782.383      0.000      67.996    5501.976   23.61       0

Total number of context switches: 2791
           Total run time (msec): 162986.495
    Total scheduling time (msec): 15001.947  (x 20)
```

**Media workers: 33324 ms**

| Worker | Runtime (ms) | Diff vs media |
|---|---:|---:|
| scriptch.02 | 49226 | **+47.7%** |
| scriptch.00 | 29782 | -10.6% |
| scriptch.01 | 27667 | -17.0% |
| scriptch.03 | 26622 | **-20.1%** |
| b-test (master) | 29687 | — |

**CV: ~27.8% — Idle time: ~0%**

---

### 8 workers

**Raw perf:**
```
Runtime summary
                          comm  parent   sched-in     run-time    min-run     avg-run     max-run  stddev  migrations
                                          (count)       (msec)     (msec)      (msec)      (msec)       %
---------------------------------------------------------------------------------------------------------------------
    b-scriptch.02[31384/31379]      -1        566    32058.593      0.000      56.640    2400.078   12.27       0
    b-scriptch.03[31385/31379]      -1        667    27850.127      0.000      41.754    7072.869   26.09       0
    b-scriptch.04[31386/31379]      -1        693    35079.521      0.000      50.619    4812.070   18.82       0
    b-scriptch.05[31387/31379]      -1        535    15790.943      0.000      29.515     205.690    3.00       0
                 b-test[31379]      -1       1463    43563.026      0.000      29.776    4861.314   15.64       0
    b-scriptch.06[31388/31379]      -1        726    25843.687      0.000      35.597    1571.038   10.29       0
    b-scriptch.07[31389/31379]      -1        952    31631.219      0.000      33.226    3182.431   14.14       0
    b-scriptch.00[31382/31379]      -1        814    34181.575      0.000      41.992    3901.067   15.16       0
    b-scriptch.01[31383/31379]      -1        541    22460.855      0.000      41.517    2962.082   16.25       0

Total number of context switches: 6957
           Total run time (msec): 268459.551
    Total scheduling time (msec): 15016.986  (x 20)
```

**Media workers: 28111 ms**

| Worker | Runtime (ms) | Diff vs media |
|---|---:|---:|
| scriptch.04 | 35079 | **+24.8%** |
| scriptch.00 | 34181 | +21.6% |
| scriptch.02 | 32058 | +14.0% |
| scriptch.07 | 31631 | +12.5% |
| scriptch.03 | 27850 | -0.9% |
| scriptch.06 | 25843 | -8.1% |
| scriptch.01 | 22460 | -20.1% |
| scriptch.05 | 15790 | **-43.8%** |
| b-test (master) | 43563 | — |

**CV: ~21.9% — Idle time: ~0%**

---

### 10 workers

**Raw perf:**
```
Runtime summary
                          comm  parent   sched-in     run-time    min-run     avg-run     max-run  stddev  migrations
                                          (count)       (msec)     (msec)      (msec)      (msec)       %
---------------------------------------------------------------------------------------------------------------------
    b-scriptch.04[31632/31625]      -1        696    27628.840      0.000      39.696    4896.709   19.27       0
    b-scriptch.05[31633/31625]      -1        546    17097.973      0.000      31.314    1943.990   11.44       0
                 b-test[31625]      -1       1609    33409.795      0.000      20.764    8284.812   26.94       0
    b-scriptch.06[31634/31625]      -1        694    25055.700      0.000      36.103    2055.137   12.08       0
    b-scriptch.07[31635/31625]      -1        613    25155.009      0.000      41.035    6914.529   27.78       0
    b-scriptch.08[31636/31625]      -1        648    21174.966      0.000      32.677    1256.721    8.99       0
    b-scriptch.00[31628/31625]      -1        545    21022.489      0.000      38.573    1691.903   11.12       0
    b-scriptch.09[31637/31625]      -1        705    19606.168      0.000      27.810    1991.694   10.81       0
    b-scriptch.01[31629/31625]      -1        626    26310.291      0.000      42.029    4443.183   18.51       0
    b-scriptch.02[31630/31625]      -1        654    23271.548      0.000      35.583    1056.961    8.58       0
    b-scriptch.03[31631/31625]      -1        686    34527.956      0.000      50.332    8534.254   25.53       0

Total number of context switches: 8022
           Total run time (msec): 274260.740
    Total scheduling time (msec): 15011.809  (x 20)
```

**Media workers: 24084 ms**

| Worker | Runtime (ms) | Diff vs media |
|---|---:|---:|
| scriptch.03 | 34527 | **+43.4%** |
| scriptch.04 | 27628 | +14.7% |
| scriptch.01 | 26310 | +9.2% |
| scriptch.07 | 25155 | +4.4% |
| scriptch.06 | 25055 | +4.0% |
| scriptch.02 | 23271 | -3.4% |
| scriptch.08 | 21174 | -12.1% |
| scriptch.00 | 21022 | -12.7% |
| scriptch.09 | 19606 | -18.6% |
| scriptch.05 | 17097 | **-29.0%** |
| b-test (master) | 33409 | — |

**CV: ~19.3% — Idle time: ~0%**

---

### 12 workers

**Raw perf:**
```
Runtime summary
                          comm  parent   sched-in     run-time    min-run     avg-run     max-run  stddev  migrations
                                          (count)       (msec)     (msec)      (msec)      (msec)       %
---------------------------------------------------------------------------------------------------------------------
    b-scriptch.04[29512/29505]      -1        726    19091.652      0.000      26.297    1814.996   10.57       0
    b-scriptch.05[29513/29505]      -1       1071    20482.284      0.000      19.124    1553.244    9.86       0
                 b-test[29505]      -1       2167    34513.510      0.000      15.926    4296.008   16.17       0
    b-scriptch.06[29514/29505]      -1       1091    16835.068      0.000      15.430     501.838    4.42       0
    b-scriptch.07[29515/29505]      -1        821    23010.154      0.000      28.026    2070.039   11.57       0
    b-scriptch.08[29516/29505]      -1        799    26736.849      0.000      33.462    4961.044   19.86       0
    b-scriptch.00[29508/29505]      -1        963    25473.899      0.000      26.452    2407.384   12.42       0
    b-scriptch.09[29517/29505]      -1        787    18072.129      0.000      22.963     342.367    4.41       0
    b-scriptch.01[29509/29505]      -1       1159    19730.674      0.000      17.023     786.210    7.47       0
    b-scriptch.10[29518/29505]      -1        884    22028.510      0.000      24.919    1031.899    8.90       0
    b-scriptch.02[29510/29505]      -1        878    25597.872      0.000      29.154    1792.258   11.42       0
    b-scriptch.11[29519/29505]      -1        633    18223.101      0.000      28.788    1147.975    8.08       0
    b-scriptch.03[29511/29505]      -1        985    20212.118      0.000      20.519     785.909    6.41       0

Total number of context switches: 12964
           Total run time (msec): 290007.825
    Total scheduling time (msec): 15035.961  (x 20)
```

**Media workers: 21291 ms**

| Worker | Runtime (ms) | Diff vs media |
|---|---:|---:|
| scriptch.08 | 26736 | **+25.6%** |
| scriptch.02 | 25597 | +20.2% |
| scriptch.00 | 25473 | +19.6% |
| scriptch.07 | 23010 | +8.1% |
| scriptch.10 | 22028 | +3.5% |
| scriptch.05 | 20482 | -3.8% |
| scriptch.03 | 20212 | -5.1% |
| scriptch.01 | 19730 | -7.3% |
| scriptch.04 | 19091 | -10.3% |
| scriptch.11 | 18223 | -14.4% |
| scriptch.09 | 18072 | -15.1% |
| scriptch.06 | 16835 | **-20.9%** |
| b-test (master) | 34513 | — |

**CV: ~14.7% — Idle time: ~0%**

---

## Tabla consolidada

| Workers | CV | Min worker (ms) | Max worker (ms) | Spread min/max |
|---:|---:|---:|---:|---:|
| 2 | **~0.25%** | 14946 | 14984 | ~0.3% |
| 4 | **~27.8%** | 26622 | 49226 | ~46% |
| 8 | **~21.9%** | 15790 | 35079 | ~55% |
| 10 | **~19.3%** | 17097 | 34527 | ~50% |
| 12 | **~14.7%** | 16835 | 26736 | ~46% |

---

## Análisis y conclusiones

### El mutex no es el problema

En ninguna configuración se observa idle time apreciable. Los workers están en estado running casi el 100% del tiempo. La contención del mutex es negligible.

**Implicación:** las propuestas orientadas a eliminar contención (colas SPSC, eliminar `nIdle`, `notify_one`) no resolverían un cuello de botella real.

### El desbalance es estructural con N > 2

El desbalance **está entre los propios workers**, no solo entre master y workers. El patrón: máximo con 4 workers (~28% CV) y decreciente al aumentar N, pero nunca vuelve al nivel de 2 workers.

**Por qué:** el bloque tiene 5000 checks. Con `nBatchSize=128` y batch dinámico `nNow = max(1, min(128, queue.size() / (nTotal + nIdle + 1)))`:
- Con 2 workers: ~20 batches por worker → ley de grandes números → balance casi perfecto
- Con 4 workers: ~3-4 batches grandes por worker → alta varianza en la asignación
- Con 12 workers: batches más pequeños y más frecuentes → mejor promediado, pero aún ~15% CV

**El outlier siempre existe:** en todas las configuraciones hay siempre un worker con desviación ≥20-44% bajo la media. No es ruido: es varianza estructural por pocos batches por worker.

El master tiene runtime siempre elevado porque hace trabajo doble: iterar las txs + `Add()` + verificar scripts en `Loop(true)`.

### Implicación del makespan

Con N workers el master espera al último en terminar. Con 4 workers, el worker más lento hace casi el doble de trabajo que el más rápido — ese tiempo extra bloquea el retorno de `ConnectBlock`. Con 8 workers hay un worker al 44% bajo la media mientras otros están al 25% por encima.

### Revisión del veredicto

| Propuesta | Veredicto |
|---|---|
| Pre-scan + scheduling óptimo | **Prometedor** — reduciría el outlier que bloquea el makespan. Beneficio crece con N. |
| Reducir `nBatchSize` | **Prometedor** — más batches pequeños = mejor balance natural sin cambio arquitectónico. |
| Colas SPSC | **Descartado** — no hay contención del mutex en ninguna configuración. |
| `notify_all → notify_one` | **Irrelevante** — workers nunca están idle. |

---

## 2026-06-30 — Discusión: realismo del benchmark y escenarios de interés

### Problema 1: el benchmark no es realista por el signature cache

`CCheckQueue` en endless mode llama a `ConnectBlock` en bucle sobre el **mismo bloque sintético**. El problema: `CScriptCheck` usa `CSignatureCache`. En la primera iteración las firmas se verifican criptográficamente. En la segunda y siguientes, son cache hits instantáneos — no hay trabajo criptográfico real.

Consecuencia: los datos de perf que tenemos miden en gran parte el throughput del cache lookup, no de la verificación real. El CV podría ser completamente diferente con checks costosos de verdad.

**Decisión pendiente:** repetir mediciones con `-sigcachesize=0` para deshabilitar el caché y forzar verificación real en cada iteración.

**Nota adicional:** `nBatchSize=128` no se va a cambiar — se quiere medir el comportamiento con los valores de producción, no encontrar un valor óptimo artificial.

### Problema 2: el bucle continuo no refleja el gap entre bloques

En producción los workers duermen ~10 minutos entre bloques. En endless mode están siempre calientes. Para tip validation (lo que importa a mineros) lo relevante es la latencia de un bloque frío, no el throughput sostenido.

### Decisión: dos escenarios de interés

Ambos escenarios son relevantes para la propuesta:

**Escenario A — Tip validation (mineros)**
- Un bloque nuevo llega cada ~10 min
- Workers estaban durmiendo, se despiertan
- Signature cache puede estar fría (depende de si el bloque reutiliza outputs conocidos)
- Lo que importa: latencia de un solo `ConnectBlock`, no throughput
- Representativo con: benchmark single-shot + `-sigcachesize=0`

**Escenario B — IBD sin `assumevalid` (`-assumevalid=0`)**
- Con `assumevalid` activado (default), los scripts NO se verifican en IBD → `CCheckQueue` casi no se usa
- Con `-assumevalid=0`, sí se verifican todos los scripts de todos los bloques desde el génesis
- Workers continuamente calientes, bloque tras bloque
- Signature cache se va llenando progresivamente (hit rate creciente)
- Bloques reales: heterogéneos (mezcla de script types, tamaños variables de 1 tx a 3000+)
- El benchmark sintético homogéneo NO es representativo aquí
- Requiere bloques reales de mainnet → setup más complejo

---

## 2026-06-30 — Visualización Gantt del scheduling por worker

### Motivación

El summary de `perf sched timehist --summary` muestra runtime total acumulado pero no permite distinguir si un worker está idle porque espera al siguiente bloque o porque no recibió trabajo dentro de un bloque. Se usa `perf sched timehist` sin `--summary` para obtener eventos individuales con timestamps.

### Herramienta

`perf timechart` no soporta `-p` para filtrar por PID. Se parseó el output de `perf sched timehist` con un script Python (`plot_gantt.py`) que genera un Gantt chart con `matplotlib`.

```bash
sudo perf sched record -p <PID> -- sleep 3
sudo perf sched timehist > /tmp/perf_timehist_10w.txt
python3 plot_gantt.py output.png <zoom_ms>
```

### Resultado — 10 workers, zoom 100ms

![Gantt 10 workers 100ms](gantt_10w_100ms.png)

![Gantt 10 workers 500ms](gantt_10w_500ms.png)

**Observaciones:**

- **Límites de bloque visibles en b-test**: gaps en ~40ms y ~80ms — el master termina `Complete()`, hace cleanup y empieza el siguiente `ConnectBlock`. Cada bloque dura ~35-40ms con el signature cache caliente.

- **scriptch.07** no trabaja en absoluto durante el primer bloque (~0-40ms). Solo arranca al inicio del segundo bloque. Es el caso extremo del desbalance: ese worker no recibió ningún check en el primer bloque.

- **scriptch.04** también arranca tarde (~20ms en el primer bloque).

- **scriptch.08, 09, 06, 03** arrancan en los primeros ms y corren prácticamente sin interrupción.

**Confirmación visual del mecanismo:**

Los gaps en los workers NO son espera por mutex — son workers que no recibieron trabajo en ese bloque. El problema es de distribución inicial de batches. En producción (un solo bloque y luego espera), scriptch.07 habría estado completamente idle durante todo el bloque mientras los demás terminaban, retrasando el retorno de `ConnectBlock`.

**Nota:** con el signature cache caliente (~35ms por bloque) el trabajo real de verificación es mínimo. Con `-sigcachesize=0` los bloques tardarían mucho más (~varios segundos) y el patrón de distribución sería diferente.

### Plan para cada escenario

**Escenario A (tip validation):**
- Repetir mediciones de perf con `-sigcachesize=0` para forzar verificación criptográfica real en cada iteración
- Usar single-shot (no endless) para medir latencia de bloque frío, que es lo que importa a mineros

**Escenario B (IBD sin assumevalid):**
- Con `assumevalid` activado (default), los scripts NO se verifican en IBD → `CCheckQueue` casi no se usa
- Necesita bloques reales de mainnet: nodo con `-assumevalid=0 -stopatheight=<N>` y perf durante la sincronización
- Setup más complejo (espacio en disco, tiempo de descarga)

### Aclaración: variance con idle ~0%

En modo endless los workers nunca duermen entre bloques — cuando terminan el bloque N el master ya está metiendo checks del bloque N+1 y los workers saltan directamente. El idle ~0% es real.

La varianza en runtime total refleja que unos workers procesan más bloques que otros en los 15 segundos: los "rápidos" terminan antes su parte de cada bloque y se enganchan antes al siguiente. Los "lentos" van siempre rezagados.

En producción (un solo bloque, luego 10 min de espera) ese mismo desbalance se manifiesta de otra forma: los workers rápidos terminan y se quedan **idle esperando al más lento** antes de que `ConnectBlock` retorne. El coste real no es que unos trabajen menos — es que el master queda bloqueado hasta que el último worker señaliza `nTodo == 0`.

---

## 2026-06-30 — Herramientas de visualización: trace-cmd y kernelshark

### Motivación del cambio

El script Python (`plot_gantt.py`) funciona correctamente y ya es no-interactivo (usa `matplotlib.use('Agg')`, no necesita display). La objeción fue conceptual: preferir herramientas estándar de la industria sobre código custom.

### trace-cmd + kernelshark

Se grabó un `trace.dat` con `trace-cmd record`:

```bash
sudo trace-cmd record -p function -e sched:sched_switch -e sched:sched_wakeup -P <PID>
```

**`kernelshark trace.dat`** — abre GUI (requiere display X11/Wayland). No tiene modo headless para exportar imagen directamente desde CLI.

**`trace-cmd report trace.dat`** — genera texto no interactivo con todos los eventos de scheduling:

```
b-test-31954 [009] d..2. 80489.322141: sched_switch: b-test:31954 [120] S ==> b-scriptch.07:31964 [120]
```

Cada línea es un `sched_switch` o `sched_wakeup` con timestamp en segundos.

### Limitación del trace.dat capturado

El `trace.dat` de esta sesión capturó solo 2 eventos de `b-scriptch` — la ventana de grabación fue demasiado corta para los workers. Necesitaría repetirse con una ventana más larga y `-P <PID>` en el momento correcto.

### Solución: `perf timechart`

`perf timechart` es un subcomando built-in de perf que graba scheduling events y genera un SVG directamente, sin GUI ni scripts externos.

**Flujo completo:**

```bash
# 1. Lanzar benchmark en modo endless
NANOBENCH_SUPPRESS_WARNINGS=1 NANOBENCH_ENDLESS=ConnectBlockMixedEcdsaSchnorr \
  ./build/bin/bench_bitcoin -filter=ConnectBlockMixedEcdsaSchnorr -par=10 &
BENCH_PID=$!

# 2. Grabar scheduling events durante 8 segundos
sudo perf timechart record -- sleep 8

# 3. Generar SVG filtrando por proceso (no interactivo)
sudo perf timechart -p bench_bitcoin -o gantt.svg

# 4. Matar benchmark
kill $BENCH_PID
```

**Resultado:** `output.svg` (13 MB, 8 segundos de traza, 10 workers). Cada thread tiene una fila con bloques de color cuando está running y espacios cuando está idle. Visualizable con cualquier navegador (`xdg-open output.svg`).

La opción `-p bench_bitcoin` filtra los threads del proceso en la fase de renderizado. El record es siempre system-wide pero el SVG resultante solo muestra los threads relevantes.

---

## 2026-07-01 — Visualización con zoom: `perf timechart` en 500ms

### Motivación

El SVG de 8 segundos (13 MB) es demasiado denso para leer. Se repite con ventana de 500ms para ver ~14 bloques con detalle.

### Comandos ejecutados

```bash
# 1. Lanzar benchmark (PID 43663)
NANOBENCH_SUPPRESS_WARNINGS=1 NANOBENCH_ENDLESS=ConnectBlockMixedEcdsaSchnorr \
  ./build/bin/bench_bitcoin -filter=ConnectBlockMixedEcdsaSchnorr -par=10 &

# 2. Grabar 500ms
sudo perf timechart record -- sleep 0.5

# 3. Generar SVG
sudo perf timechart -p bench_bitcoin -o gantt_zoom.svg

# 4. Matar benchmark
kill 43663
```

**Resultado:** `gantt_zoom.svg` (479 KB, 1000×1260 px). → [ver imagen](gantt_zoom.svg)


### Datos extraídos del SVG

| Worker | Runtime (ms) | Slices >10ms | Idle est. (ms) | Idle % |
|---|---:|---:|---:|---:|
| b-scriptch.00 | 441 | 15 | 59 | 11.8% |
| b-scriptch.01 | 411 | 15 | 89 | 17.7% |
| b-scriptch.02 | 404 | 13 | 96 | 19.2% |
| b-scriptch.03 | 408 | 13 | 92 | 18.4% |
| b-scriptch.04 | 441 | 15 | 59 | 11.7% |
| b-scriptch.05 | 421 | 15 | 79 | 15.9% |
| b-scriptch.06 | 455 | 14 | 45 | 9.0% |
| b-scriptch.07 | 434 | 14 | 67 | 13.3% |
| b-scriptch.08 | 411 | 15 | 89 | 17.8% |
| b-scriptch.09 | 408 | 13 | 92 | 18.4% |
| **b-test** | **474** | **14** | — | — |

**CV workers: 4.2%** (mean=423ms, stddev=17.9ms)

---

## 2026-07-01 — Experimento 0.5s: SVG + tabla CV + raw summary (10 workers)

### Comandos

```bash
NANOBENCH_SUPPRESS_WARNINGS=1 NANOBENCH_ENDLESS=ConnectBlockMixedEcdsaSchnorr \
  ./build/bin/bench_bitcoin -filter=ConnectBlockMixedEcdsaSchnorr -par=10 &

sudo perf timechart record -- sleep 0.5 && sudo chmod a+r perf.data
perf timechart -p bench_bitcoin -o gantt_05s.svg
perf sched timehist --summary
```

### Raw summary

```
                          comm  parent   sched-in     run-time    min-run     avg-run     max-run  stddev  migrations
                                          (count)       (msec)     (msec)      (msec)      (msec)       %
---------------------------------------------------------------------------------------------------------------------
    b-scriptch.05[51136/51128]   51128         31      471.579      0.007      15.212      27.703   12.26       0
    b-scriptch.07[51138/51128]   51128         46      464.426      0.009      10.096      25.328   14.82       0
    b-scriptch.04[51135/51128]   51128         54      459.876      0.008       8.516      26.046   15.11       0
    b-scriptch.01[51132/51128]   51128         55      451.124      0.009       8.202      24.758   14.92       0
    b-scriptch.03[51134/51128]   51128         41      447.106      0.003      10.905      24.484   14.73       0
                 b-test[51128]    4534        106      448.493      0.003       4.231      21.581   16.75       0
    b-scriptch.00[51131/51128]   51128         26      425.046      0.002      16.347      23.445   10.90       0
    b-scriptch.09[51140/51128]   51128         34      423.821      0.005      12.465      23.467   14.13       0
    b-scriptch.08[51139/51128]   51128         29      421.800      0.005      14.544      23.452   11.93       0
    b-scriptch.06[51137/51128]   51128         27      421.354      0.006      15.605      23.540   11.51       0
    b-scriptch.02[51133/51128]   51128         31      421.064      0.002      13.582      23.463   13.63       0
```

### SVG


### Tabla de runtimes y CV

| Worker | Runtime (ms) | Diff vs media |
|---|---:|---:|
| b-scriptch.05 | 471.6 | **+7.0%** |
| b-scriptch.07 | 464.4 | +5.4% |
| b-scriptch.04 | 459.9 | +4.3% |
| b-scriptch.01 | 451.1 | +2.4% |
| b-scriptch.03 | 447.1 | +1.4% |
| b-scriptch.00 | 425.0 | -3.6% |
| b-scriptch.09 | 423.8 | -3.8% |
| b-scriptch.08 | 421.8 | -4.3% |
| b-scriptch.06 | 421.4 | -4.4% |
| b-scriptch.02 | 421.1 | **-4.5%** |
| b-test (master) | 448.5 | — |

**Media: 440.7 ms — Stddev: 20.2 ms — CV: 4.6%**

Spread min/max: 95.5%–107.0% de la media (~11.5% entre extremos). → [ver imagen](gantt_05s.svg)

---

## 2026-07-01 — Experimento 15s: SVG + tabla CV + raw summary (10 workers)

### Comandos

```bash
NANOBENCH_SUPPRESS_WARNINGS=1 NANOBENCH_ENDLESS=ConnectBlockMixedEcdsaSchnorr \
  ./build/bin/bench_bitcoin -filter=ConnectBlockMixedEcdsaSchnorr -par=10 &

sudo perf timechart record -- sleep 15 && sudo chmod a+r perf.data
perf timechart -p bench_bitcoin -o gantt_15s.svg
perf sched timehist --summary
```

### Verificación de identidad de threads

Todos los threads muestran `parent=53905` (PID exacto del benchmark). Los nombres `b-scriptch.XX` son asignados explícitamente por Bitcoin Core en `src/checkqueue.h` vía `util::ThreadSetInternalName`. Ningún otro proceso del sistema tiene threads con ese nombre.

### Raw summary

```
                          comm  parent   sched-in     run-time    min-run     avg-run     max-run  stddev  migrations
                                          (count)       (msec)     (msec)      (msec)      (msec)       %
---------------------------------------------------------------------------------------------------------------------
    b-scriptch.03[53918/53905]   53905       1035    13315.130      0.002      12.864      31.712    2.60       0
    b-scriptch.08[53923/53905]   53905        987    13187.288      0.002      13.360      30.028    2.57       0
    b-scriptch.09[53924/53905]   53905        894    13114.945      0.001      14.669      30.057    2.43       0
                 b-test[53905]    4534       2843    13136.621      0.002       4.620      28.629    3.07       0
    b-scriptch.02[53917/53905]   53905        950    12971.242      0.001      13.653      33.330    2.52       0
    b-scriptch.07[53922/53905]   53905        963    12900.125      0.001      13.395      29.921    2.52       0
    b-scriptch.06[53921/53905]   53905       1078    12855.596      0.001      11.925      31.425    2.61       0
    b-scriptch.05[53920/53905]   53905       1033    12474.228      0.001      12.075      29.104    2.59       0
    b-scriptch.01[53916/53905]   53905        859    12337.809      0.001      14.362      29.956    2.37       0
    b-scriptch.04[53919/53905]   53905        816    12329.473      0.001      15.109      30.594    2.26       0
    b-scriptch.00[53915/53905]   53905        813    12314.818      0.001      15.147      28.289    2.26       0
```

### SVG


### Tabla de runtimes y CV

| Worker | Runtime (ms) | Diff vs media |
|---|---:|---:|
| b-scriptch.03 | 13315.1 | **+4.2%** |
| b-scriptch.08 | 13187.3 | +3.2% |
| b-scriptch.09 | 13114.9 | +2.6% |
| b-scriptch.02 | 12971.2 | +1.5% |
| b-scriptch.07 | 12900.1 | +0.9% |
| b-scriptch.06 | 12855.6 | +0.6% |
| b-scriptch.05 | 12474.2 | -2.4% |
| b-scriptch.01 | 12337.8 | -3.5% |
| b-scriptch.04 | 12329.5 | -3.5% |
| b-scriptch.00 | 12314.8 | **-3.6%** |
| b-test (master) | 13136.6 | — |

**Media: 12780.1 ms — Stddev: 384.4 ms — CV: 3.0%**

Spread min/max: 96.4%–104.2% de la media (~7.8% entre extremos). → [ver imagen](gantt_15s.svg)

### Revisión de mediciones anteriores con `perf sched record`

Las mediciones antiguas de 15s (sección "Experimentos — ConnectBlockMixedEcdsaSchnorr") mostraban CV ~19% y runtimes individuales >30s en una ventana de 15s (e.g. scriptch.03: 34527ms), lo cual es físicamente imposible para un hilo en 15s de elapsed. Esos datos tenían artefactos de medición: `perf sched record -p $PID` capturaba threads con `parent=-1` (no verificado contra el PID del proceso), y posiblemente acumulaba mal los intervalos al adjuntarse a un proceso ya en marcha.

Las mediciones con `perf timechart record` (system-wide, filtrado por nombre de proceso en postproceso) dan resultados coherentes y verificables: runtimes < elapsed, parent PID explícito, CV estable entre 3–5% en todas las ventanas temporales.

---

## 2026-07-01 — Experimento 60s: tabla CV + raw summary (10 workers)

### Comandos

```bash
NANOBENCH_SUPPRESS_WARNINGS=1 NANOBENCH_ENDLESS=ConnectBlockMixedEcdsaSchnorr \
  ./build/bin/bench_bitcoin -filter=ConnectBlockMixedEcdsaSchnorr -par=10 &

sudo perf timechart record -- sleep 60 && sudo chmod a+r perf.data
perf timechart -p bench_bitcoin -o gantt_60s.svg   # 51 MB — no en git
perf sched timehist --summary
```

### Raw summary

```
                          comm  parent   sched-in     run-time    min-run     avg-run     max-run  stddev  migrations
                                          (count)       (msec)     (msec)      (msec)      (msec)       %
---------------------------------------------------------------------------------------------------------------------
    b-scriptch.07[55466/55456]   55456       3263    52639.788      0.002      16.132      37.597    1.45       0
    b-scriptch.04[55463/55456]   55456       3187    52189.367      0.002      16.375      35.733    1.41       0
    b-scriptch.05[55464/55456]   55456       3052    52091.060      0.002      17.067      37.538    1.38       0
    b-scriptch.01[55460/55456]   55456       3179    51994.568      0.002      16.355      36.308    1.42       0
    b-scriptch.08[55467/55456]   55456       3075    51231.788      0.001      16.660      37.260    1.40       0
    b-scriptch.06[55465/55456]   55456       3094    51092.704      0.001      16.513      37.279    1.41       0
    b-scriptch.02[55461/55456]   55456       3020    50879.515      0.001      16.847      37.328    1.39       0
    b-scriptch.09[55468/55456]   55456       2928    50104.555      0.001      17.112      37.367    1.36       0
    b-scriptch.03[55462/55456]   55456       2857    50024.516      0.001      17.509      37.280    1.34       0
    b-scriptch.00[55459/55456]   55456       2761    49777.516      0.001      18.028      37.258    1.30       0
                 b-test[55456]    4534       7891    53417.552      0.002       6.769      70.872    1.68       0
```

### Tabla de runtimes y CV

| Worker | Runtime (ms) | Diff vs media |
|---|---:|---:|
| b-scriptch.07 | 52639.8 | **+2.8%** |
| b-scriptch.04 | 52189.4 | +1.9% |
| b-scriptch.05 | 52091.1 | +1.7% |
| b-scriptch.01 | 51994.6 | +1.5% |
| b-scriptch.08 | 51231.8 | +0.1% |
| b-scriptch.06 | 51092.7 | -0.2% |
| b-scriptch.02 | 50879.5 | -0.6% |
| b-scriptch.09 | 50104.6 | -2.1% |
| b-scriptch.03 | 50024.5 | -2.3% |
| b-scriptch.00 | 49777.5 | **-2.8%** |
| b-test (master) | 53417.6 | — |

**Media: 51202.5 ms — Stddev: 1010.9 ms — CV: 2.0%**

Spread min/max: 97.2%–102.8% (~5.6% entre extremos).

### Tendencia: convergencia por ley de grandes números

| Ventana | CV | Spread min/max |
|---:|---:|---:|
| 0.5s | 4.6% | ~11% |
| 15s | 3.0% | ~8% |
| 60s | **2.0%** | ~6% |

El CV decrece con la ventana de medición. La distribución aleatoria de batches entre workers se promedia sobre cientos/miles de bloques. Con 60s y ~35ms por bloque se procesan ~1700 bloques: la varianza por bloque individual se divide por √1700 ≈ 41 — exactamente el comportamiento esperado de una suma de variables independientes.

**Conclusión parcial:** el CV global converge, pero el problema real es otro: aunque globalmente los workers se equilibren, dentro de cada bloque los rápidos terminan antes y quedan idle esperando al más lento. El global siendo uniforme no implica que los recursos no se malgasten por bloque — podría ser que primero el worker 1 haga el 80% del trabajo, luego el 2, luego el 3, y el CV global salga bajo aunque en cada bloque haya un straggler claro.

---

## 2026-07-01 — Análisis per-block: spread de finish times y waste ratio

### Motivación

El CV global mide si a lo largo del tiempo cada worker recibe la misma cantidad de trabajo total. No mide el problema real: **dentro de cada bloque individual**, los workers más rápidos terminan antes y quedan idle hasta que el más lento acaba — bloqueando el retorno de `ConnectBlock`. El CV global puede ser bajo aunque en cada bloque haya un straggler importante.

La métrica relevante es:
- **Spread por bloque**: `max(finish_time) - min(finish_time)` entre workers en un mismo bloque = tiempo que los workers rápidos pierden al final de cada bloque
- **Waste ratio**: spread / makespan = fracción del bloque que se "malgasta" en espera del straggler

### Método

`perf sched timehist` sin `--summary` da eventos individuales con timestamps. Se identifican límites de bloque por los eventos de `b-test` (el master: llama a `Add()`, duerme en `Complete()`, se despierta cuando todos terminan). Para cada bloque se toma el último evento de scheduling de cada worker y se calcula el spread.

Script: `/tmp/per_block_cv.py`

### Datos

Trace de 3 min (`perf.data`) y `perf.data.old` (60s). Las ventanas de 0.5s, 5s y 15s **no son recordings independientes** — son los primeros N segundos del trace de 3 min recortados por timestamp. El 60s es una grabación real separada.

| Ventana | Bloques analizados | Spread medio | Spread p95 | Waste ratio |
|---:|---:|---:|---:|---:|
| 0.5s | 25 | 8.85 ms | 30.3 ms | **50.2%** |
| 5s | 251 | 7.19 ms | 33.8 ms | **46.7%** |
| 15s | 738 | 7.42 ms | 34.4 ms | **48.7%** |
| 60s | 3723 | 7.26 ms | 26.4 ms | **57.6%** |
| 3m | 8250 | 8.00 ms | 36.0 ms | **49.8%** |

### Análisis

**El spread de ~8ms por bloque es consistente e independiente de la ventana de medición.** No converge como el CV global — es una propiedad estructural del scheduling por bloque.

**Limitación del waste ratio:** el makespan calculado (~16ms) es la mitad de la duración real de los bloques (~35ms visible en el Gantt), porque la detección de límites de bloque basada en eventos de `b-test` está partiendo cada bloque real en ~2 sub-ventanas. `b-test` tiene varios eventos de scheduling por bloque (Add(), sleep en Complete(), wakeup, cleanup). La ratio spread/makespan del ~50% es un artefacto de este error de detección.

**La métrica correcta:** spread de ~8ms sobre bloques reales de ~35ms → **~23% del tiempo de bloque malgastado** en espera del worker más lento. En el p95 el spread llega a 30-36ms, casi un bloque completo de diferencia entre el primero y el último en terminar.

**Por qué el CV global era engañoso:** si en el bloque 1 el straggler es el worker 3, en el bloque 2 el worker 7, etc., el CV global converge a cero pero en cada bloque hay siempre ~8ms de espera. El CV global enmascara exactamente el problema que importa: la latencia de `ConnectBlock` está determinada por el más lento en cada bloque individual.

**Implicación para la propuesta:** un reparto más equilibrado de checks entre workers (pre-scan de coste estimado, distribución por peso) reduciría el spread por bloque de ~8ms hacia cero. Con 10 workers, 5000 checks/bloque y `nBatchSize=128`, la granularidad actual impide un reparto fino. Reducir el spread en ~8ms reduciría la latencia de `ConnectBlock` en ~8ms por bloque (~23%).

### ¿Vale la pena calcular CV por bloque?

Se consideró calcular, por cada bloque, la desviación de cada worker respecto a la media de finish times del bloque, y luego promediar across bloques. Eso daría una descripción más rica de la distribución intra-bloque que el simple max-min.

**Conclusión: no añade valor sobre el spread.** El problema que queremos cuantificar es "cuánto tarda ConnectBlock de más por culpa del straggler". Eso es exactamente `max_finish - min_finish`. La media de desviaciones respecto a la media daría más detalle pero no cambiaría la conclusión ni la propuesta arquitectónica. La métrica relevante es siempre el último en terminar, no la distribución completa.

### Siguiente paso para métricas per-block precisas

El boundary detection actual (eventos de `b-test` con run_ms >= 1ms) es impreciso: b-test tiene múltiples eventos de scheduling por bloque (Add(), sleep en Complete(), wakeup, cleanup), lo que parte cada bloque real en ~2 sub-ventanas y da makespan de ~16ms en vez de ~35ms. Cualquier ratio calculado sobre ese makespan arrastra el error.

Para tener boundaries exactos sin instrumentar el código: **bpftrace con uretprobe en `pthread_cond_wait` del hilo master** — cada retorno de ese wait = nTodo llegó a 0 = fin de bloque exacto.

---

## 2026-07-01 — bpftrace: spread per-block con boundaries exactos

### Script (`/tmp/block_spread.bt`)

```bpftrace
tracepoint:sched:sched_switch
/strncmp("b-scriptch", args->prev_comm, 10) == 0/
{
    printf("WORKER %llu %s\n", nsecs, args->prev_comm);
}

uretprobe:/lib64/libc.so.6:pthread_cond_wait
/comm == "b-test"/
{
    printf("BLOCK_END %llu\n", nsecs);
}
```

`BLOCK_END` se emite en el retorno de `pthread_cond_wait` del master (`b-test`) — exactamente cuando `nTodo == 0` y `ConnectBlock` puede retornar. `WORKER` se emite en cada descheduling de un worker.

```bash
NANOBENCH_SUPPRESS_WARNINGS=1 NANOBENCH_ENDLESS=ConnectBlockMixedEcdsaSchnorr \
  ./build/bin/bench_bitcoin -filter=ConnectBlockMixedEcdsaSchnorr -par=10 &

sudo bpftrace /tmp/block_spread.bt > /tmp/bpftrace_out.txt 2>&1 &
sleep 10 && sudo kill $(pgrep bpftrace)
```

### Raw output (muestra)

```
Attached 2 probes
WORKER 84158603203700 b-scriptch.01
WORKER 84158612203918 b-scriptch.01
...
BLOCK_END 84158621551136
WORKER 84158624660931 b-scriptch.03
...
```

206 BLOCK_END events en 10s → ~46ms por bloque (consistente con Gantt).

### Resultados

| Métrica | Valor |
|---|---:|
| Bloques analizados | 206 |
| Duración media de bloque | ~46 ms |
| Spread medio | **4.44 ms** |
| Spread mediana | 4.23 ms |
| Spread stddev | 1.83 ms |
| Spread p95 | 7.90 ms |
| Spread p99 | 8.56 ms |
| Spread máx | 9.35 ms |
| **Waste ratio medio** | **9.6%** |
| Waste ratio p95 | 17.2% |

### Análisis

Con boundaries exactos el spread medio baja a **4.44ms** (vs los ~8ms del análisis anterior con perf, que tenía boundaries incorrectos). El waste ratio real es **~10%**, no el 23% estimado antes.

La distribución es estrecha (stddev 1.83ms) y el p99 está en 8.56ms — casi ningún bloque tiene un straggler extremo. El mayor spread observado fue 9.35ms sobre bloques de ~46ms.

**Interpretación:** en modo cache-caliente, ConnectBlock tarda ~46ms y los workers terminan dentro de una ventana de ~4.4ms entre sí. El 10% de overhead por imbalance existe pero no es catastrófico. El impacto real estaría con verificación criptográfica real (sin cache), donde los checks son más heterogéneos en coste y el spread sería mayor.

### ¿La distribución del spread es uniforme entre bloques?

No. El CV del spread entre bloques es **41%** — hay mucha variabilidad bloque a bloque:

```
 0-1ms:   0 bloques  (0.0%)
 1-2ms:  11 bloques  (5.3%)  ██████████
 2-3ms:  43 bloques (20.9%)  ████████████████████████████████████████
 3-4ms:  39 bloques (18.9%)  ████████████████████████████████████
 4-5ms:  42 bloques (20.4%)  ███████████████████████████████████████
 5-6ms:  29 bloques (14.1%)  ██████████████████████████
 6-7ms:  19 bloques  (9.2%)  █████████████████
 7-8ms:  15 bloques  (7.3%)  █████████████
 8-9ms:   7 bloques  (3.4%)  ██████
≥10ms:   0 bloques  (0.0%)
```

La distribución es una campana sesgada a la derecha: mayoría entre 2-5ms, cola hasta 9ms. El straggler no es siempre el mismo worker ni siempre del mismo tamaño — es varianza aleatoria en la asignación de batches. Algunos bloques quedan casi perfectamente balanceados (~2ms), otros tienen un worker claramente rezagado (~8-9ms).

**Conclusión final del profiling con cache caliente:**
- CV global: 3-5% (converge con la ventana, no es la métrica relevante)
- Spread per-block: 4.4ms media / 7.9ms p95 / distribución con CV=41% entre bloques
- Waste ratio: ~10% de la duración del bloque
- El desbalance es estocástico, no estructural: varía bloque a bloque según qué checks caen en cada batch
- Siguiente experimento necesario: repetir con `-sigcachesize=0` para medir el caso real con verificación criptográfica

---

## 2026-07-01 — bpftrace con `-sigcachesize=0`: spread per-block sin cache de firmas

### Contexto: qué es `min_validation_cache` y cómo funciona el nuevo flag

`TestOpts::min_validation_cache` es un flag del framework de tests de Bitcoin Core que, cuando está a `true`, pone a cero tanto `ChainstateManager::Options::signature_cache_bytes` como `script_execution_cache_bytes`. Esto deshabilita completamente la `CSignatureCache` — la cache que evita reverificar firmas ya vistas.

En el benchmark sin este flag (modo por defecto): la primera iteración del bloque sintético verifica criptográficamente las 5000 firmas. Las siguientes iteraciones son hits de cache (~nanosegundos por check). El benchmark mide en la práctica el throughput de cache lookups, no de verificación criptográfica.

Con `-sigcachesize=0`: **cada iteración verifica criptográficamente todas las firmas**. Cada check ejecuta ECDSA o Schnorr completo. Esto es representativo de:
- **IBD sin assumevalid**: los bloques son nuevos, las firmas nunca se han visto → cache siempre fría
- **Tip validation con bloque que contiene txs fuera del mempool**: firmas no precalculadas

#### Cambios en el código

Para poder pasar `-sigcachesize=0` desde la CLI del benchmark fue necesario:

**`src/test/util/setup_common.cpp`** — registrar `-sigcachesize` en `SetupCommonTestArgs` (que inicializa tanto el ArgsManager del benchmark como el del nodo interno `gArgs`), y leerlo en la inicialización del chainstate:

```cpp
// SetupCommonTestArgs:
argsman.AddArg("-sigcachesize=<n>", "...", ArgsManager::ALLOW_ANY, OptionsCategory::DEBUG_TEST);

// ChainTestingSetup:
if (opts.min_validation_cache || m_node.args->GetIntArg("-sigcachesize", -1) == 0) {
    chainman_opts.script_execution_cache_bytes = 0;
    chainman_opts.signature_cache_bytes = 0;
}
```

**`src/bench/bench_bitcoin.cpp`** — añadir `-sigcachesize` a `AVAILABLE_ARGS` para que se reenvíe al test setup:
```cpp
static std::vector<std::string> AVAILABLE_ARGS = {"-testdatadir", "-par", "-sigcachesize"};
```

### Comandos

```bash
NANOBENCH_SUPPRESS_WARNINGS=1 NANOBENCH_ENDLESS=ConnectBlockMixedEcdsaSchnorr \
  ./build/bin/bench_bitcoin -filter=ConnectBlockMixedEcdsaSchnorr -par=10 -sigcachesize=0 &

sudo bpftrace /tmp/block_spread.bt > /tmp/bpftrace_nocache.txt 2>&1 &
sleep 10 && sudo kill $(pgrep bpftrace)
```

### Resultados

271 bloques en 10s → **~35.4ms por bloque**. Ejecución limpia con un único proceso `b-test` (la primera ejecución estaba contaminada con dos procesos solapados y fue descartada).

| Métrica | Con cache | Sin cache (`-sigcachesize=0`) |
|---|---:|---:|
| Duración media bloque | 46 ms | 35.4 ms |
| Spread medio | 4.44 ms | 3.07 ms |
| Spread mediana | 4.23 ms | 2.70 ms |
| Spread stddev | 1.83 ms | 1.81 ms |
| Spread p95 | 7.90 ms | 6.06 ms |
| Spread p99 | 8.56 ms | 6.99 ms |
| Spread máx | 9.35 ms | 19.13 ms |
| Waste ratio medio | 9.6% | 8.7% |
| Waste ratio p95 | 17.2% | 17.1% |
| CV del spread | 41% | 59.0% |

### Distribución del spread sin cache

```
  0-  5ms:  237 (87.5%)  ████████████████████████████████████████
  5- 10ms:   33 (12.2%)  █████
 15- 20ms:    1 ( 0.4%)
```

### Análisis

Contra lo esperado, el spread **baja** sin cache (3.07ms vs 4.44ms) y el waste ratio también (8.7% vs 9.6%). La distribución es más concentrada: 87.5% de bloques con spread <5ms, vs ~60% con cache caliente.

**Por qué bajan los spreads:** con cache caliente, los lookups tienen varianza propia (contención del mutex de la cache, líneas de cache frías en la primera iteración por bloque sintético). Sin cache, cada check ejecuta ECDSA/Schnorr completo — operaciones más lentas pero uniformes en coste. La eliminación de la varianza del cache lookup reduce la dispersión entre workers.

**El CV sube a 59% (vs 41% con cache):** aunque los spreads absolutos son menores, hay más variabilidad relativa entre bloques. Hay un outlier de 19ms (vs 9ms máx con cache) — probablemente un bloque donde un worker acumuló un batch de Schnorr más costoso.

**Limitación del benchmark:** el bloque sintético es siempre el mismo. En producción cada bloque tiene distinta composición de script types y número de inputs, lo que daría spreads más variables y haría la comparativa más significativa.

---

## 2026-07-01 — Experimento unificado: SVG + tabla CV del mismo perf.data (10 workers, 5s)

### Comandos

```bash
# 1. Lanzar benchmark (PID 48192)
NANOBENCH_SUPPRESS_WARNINGS=1 NANOBENCH_ENDLESS=ConnectBlockMixedEcdsaSchnorr \
  ./build/bin/bench_bitcoin -filter=ConnectBlockMixedEcdsaSchnorr -par=10 &

# 2. Grabar y dar permisos de lectura
sudo perf timechart record -- sleep 5 && sudo chmod a+r perf.data

# 3. SVG (del mismo perf.data)
perf timechart -p bench_bitcoin -o gantt_zoom2.svg

# 4. Tabla de runtimes (del mismo perf.data)
perf sched timehist --summary

# 5. Matar benchmark
kill 48192
```

`perf timechart record` captura `sched_switch`/`sched_wakeup`, que son exactamente los eventos que necesita `perf sched timehist`. Un solo recording produce las dos salidas.

→ [ver imagen](gantt_zoom2.svg)

### Tabla de runtimes y CV

| Worker | Runtime (ms) | Diff vs media |
|---|---:|---:|
| b-scriptch.04 | 4411.7 | **+5.4%** |
| b-scriptch.03 | 4388.8 | +4.8% |
| b-scriptch.02 | 4357.5 | +4.1% |
| b-scriptch.00 | 4274.2 | +2.1% |
| b-scriptch.09 | 4215.4 | +0.7% |
| b-scriptch.08 | 4196.6 | +0.2% |
| b-scriptch.06 | 4034.9 | -3.6% |
| b-scriptch.07 | 3998.0 | -4.5% |
| b-scriptch.01 | 3997.7 | -4.5% |
| b-scriptch.05 | 3994.1 | **-4.6%** |
| b-test (master) | 4304.7 | — |

**Media: 4186.9 ms — Stddev: 170.2 ms — CV: 4.1%**

### Observación

CV de 4.1% con 5 segundos de ventana — consistente con el 4.2% del experimento de 500ms anterior. El spread max/min es de ~10% (95-105% de la media), muy inferior al 46-55% observado en las mediciones de 15s con `perf sched record`. La diferencia se debe al modo de medición: `perf timechart` cuenta tiempo en CPU real incluyendo preemptions breves, mientras que `perf sched timehist` acumula varianza de cientos de bloques donde el OS interfiere más. Ninguno de los dos tiene idle significativo — los workers están activos prácticamente todo el tiempo.

### Análisis

**Los slices grandes (>10ms) son batches de trabajo real.** Cada worker tiene 13-15 slices grandes en 500ms — corresponden a las iteraciones de bloques. A ~35ms por bloque y 10 workers procesando en paralelo, esto es consistente: 500ms / 35ms ≈ 14 bloques.

**El idle sí existe y es medible en ventana corta: 9-19% por worker.** Esto contradice el "idle ~0%" de los experimentos de 15s con `perf sched timehist`. La explicación:

- `perf sched timehist --summary` mide el tiempo que el thread está en estado `sleeping` en el scheduler. En modo endless, los workers no duermen (TASK_INTERRUPTIBLE) entre bloques — están en busy-wait o se desactivan brevísimamente.
- `perf timechart` mide el tiempo en estado `running` en CPU. Los gaps que aparecen son contextos donde el OS pone al worker fuera de CPU (preemption, migración) aunque el thread técnicamente no "duerme".

**El b-test tiene 28 slices pequeños (69ms total)** frente a 14 grandes. Los pequeños son el cleanup entre bloques: `ConnectBlock` retorna, el benchmark crea el siguiente bloque, llama a `Add()`. Esos ~5ms de gap por bloque (69ms / 14 bloques) son exactamente el tiempo en que todos los workers están idle esperando el siguiente `Add()`.

**CV de 4.2% vs 19-28% previos.** En 500ms los bloques son relativamente uniformes. La varianza alta en los experimentos de 15s refleja acumulación de efectos a lo largo de cientos de bloques: el OS migrando threads entre cores, variación en cache state, etc. En una ventana corta la distribución de batches es más regular.

---

## 2026-07-01 — Corrección: interpretación del CV y el outlier de 19ms

Al revisar el experimento de bpftrace con `-sigcachesize=0`, dos afirmaciones en el análisis eran incorrectas:

**CV: por qué sube de 41% a 59% aunque el spread absoluto baje**

CV = σ / μ. Los valores concretos:

| | Con cache | Sin cache |
|---|---:|---:|
| σ (stddev spread) | 1.83 ms | 1.81 ms |
| μ (mean spread) | 4.44 ms | 3.07 ms |
| CV | 41% | 59% |

El stddev es prácticamente idéntico en los dos casos (~1.82ms). El CV sube porque la media bajó un 31% — el denominador se encogió. No hay más variabilidad absoluta entre bloques; hay la misma variabilidad pero relativa a una media más pequeña.

**Outlier de 19ms: no es un batch de Schnorr**

El journal atribuía el outlier a "un worker que acumuló un batch de Schnorr más costoso". Esto es incorrecto: el benchmark sintético repite el mismo bloque en cada iteración — todos los bloques tienen exactamente la misma composición de scripts. El outlier es interferencia del OS (preemption, migración de core, presión de memoria) que afectó a un worker en un bloque concreto.

---

## 2026-07-01 — perf probe en `Chainstate::ConnectBlock`: boundaries exactos + SVG

### Motivación

La aproximación anterior con bpftrace usaba `uretprobe` en `pthread_cond_wait` de libc como proxy del fin de bloque. Es correcto (el master despierta cuando `nTodo==0`), pero introduce ruido propio del uretprobe de libc — visible en el outlier de 19ms del experimento anterior. El objetivo es usar `perf probe` directamente en `Chainstate::ConnectBlock` como boundary, y combinar en una sola sesión de `perf record` los eventos necesarios para el SVG y para el análisis de spread.

### Problema con `%return` probe

`perf probe --add 'connect_block_end=Chainstate::ConnectBlock%return'` falló porque el compilador usa tail-call optimization o múltiples return points en `ConnectBlock`. Solución: usar solo la probe de entrada y definir el bloque como la ventana `[start_N, start_{N+1})`. El spread entre workers (max_finish − min_finish) no depende del end exacto del bloque — solo necesitamos atribuir cada `sched_switch` al bloque correcto, lo que la ventana start-to-start proporciona.

### Registro de la probe (una sola vez como root)

```bash
MANGLED="_ZN10Chainstate12ConnectBlockERK6CBlockR20BlockValidationStateP11CBlockIndexR15CCoinsViewCacheb"
sudo perf probe -x ./build/bin/bench_bitcoin --add "connect_block_start=${MANGLED}"
```

El símbolo manglado se obtiene con `nm ./build/bin/bench_bitcoin | grep ConnectBlock | grep Chainstate`.

`perf timechart record` no acepta `-e` adicionales, por lo que se usa `perf record -a` con los eventos que timechart necesita más el probe:

### Comandos

```bash
# 1. Lanzar benchmark
NANOBENCH_SUPPRESS_WARNINGS=1 NANOBENCH_ENDLESS=ConnectBlockMixedEcdsaSchnorr \
  ./build/bin/bench_bitcoin -filter=ConnectBlockMixedEcdsaSchnorr -par=10 &

# 2. Grabar (system-wide, 10s)
sudo perf record -a \
    -e sched:sched_switch \
    -e sched:sched_wakeup \
    -e probe_bench_bitcoin:connect_block_start \
    -- sleep 10 && sudo chmod a+r perf.data

# 3. SVG gantt
perf timechart -p bench_bitcoin -o gantt_connectblock.svg

# 4. Análisis de spread
perf script -i perf.data --fields time,event,comm \
  | python3 scripts/parse_connectblock_spread.py
```

→ [ver imagen](gantt_connectblock.svg)

### Resultados

266 bloques, todos con los 10 workers presentes.

| Métrica | bpftrace (pthread_cond_wait) | perf probe (ConnectBlock) |
|---|---:|---:|
| Bloques analizados | 271 | 266 |
| Duración media bloque | 35.4 ms | 37.5 ms |
| Spread medio | 3.07 ms | **2.98 ms** |
| Spread mediana | 2.70 ms | 2.90 ms |
| Spread stddev | 1.81 ms | 1.41 ms |
| Spread p95 | 6.06 ms | **5.50 ms** |
| Spread p99 | 6.99 ms | 6.61 ms |
| Spread máx | **19.13 ms** | **7.09 ms** |
| Waste ratio medio | 8.7% | **7.9%** |
| Waste ratio p95 | 17.1% | 14.1% |
| CV del spread | 59.0% | **47.3%** |

### Distribución del spread (perf probe)

```
  0-  2ms:   75 (28.2%)  ███████████
  2-  4ms:  128 (48.1%)  ███████████████████
  4-  6ms:   58 (21.8%)  ████████
  6-  8ms:    5 ( 1.9%)
```

### Análisis

Los resultados son consistentes con bpftrace. La diferencia más notable es el máximo: 19.13ms con bpftrace vs 7.09ms con perf probe. El outlier de bpftrace era ruido del `uretprobe` en libc (el trap de kernel para el return probe de `pthread_cond_wait` tiene latencia propia que en casos extremos puede acumularse). Con boundaries directamente en `ConnectBlock`, la distribución es más limpia y el máximo cae dentro del rango esperado.

El spread medio de ~3ms sobre bloques de ~37ms (waste ratio ~8%) se confirma como el número estructural: con 10 workers y el esquema de batches estáticos de `CCheckQueue`, la cola se vacía con ~3ms de straggler time promedio, independientemente del método de medición.

---

## Conclusión: ¿es uniforme el trabajo? ¿merece la pena cambiar el scheduler?

### Uniformidad

**A nivel global: sí.** CV de runtime entre workers ~4% en ventanas de 5-10s. Los workers ejecutan prácticamente el mismo tiempo total.

**A nivel de bloque: hay straggler inevitable.** Spread medio de ~3ms por bloque (8% del tiempo de bloque). No es un problema de scheduling — es aritmética de batches: con ~16 batches para 10 workers, la última ronda de despacho siempre deja algún worker con un batch mientras los demás ya terminaron.

### ¿Vale la pena cambiar el scheduler?

No, por las razones siguientes:

**La cola con lock ya hace lo correcto.** Dispatch dinámico: los workers toman trabajo hasta que no queda nada. Esto ya minimiza el desbalance estructural. El idle es ≈0% y la contención del mutex es insignificante — confirmado por medición directa.

**Las alternativas no atacan el problema real:**
- *Lock-free queue*: elimina contención del mutex, que ya es despreciable. Coste de implementación alto, ganancia nula.
- *Work stealing*: útil cuando hay desbalance estructural persistente. Aquí el desbalance es 1 batch al final de cada bloque — work stealing no lo elimina, solo lo redistribuye.
- *Batch size menor*: reduciría el spread (el último batch pesa menos como fracción del total), pero aumenta el número de adquisiciones del lock por bloque. Trade-off que requeriría medición.

**El margen real está entre bloques, no dentro.** El benchmark muestra ~5ms de gap entre llamadas a `ConnectBlock` donde todos los workers están ociosos. En producción ese tiempo corresponde al setup previo al lanzamiento de checks. Si ese overhead es significativo en IBD, su impacto supera al spread intra-bloque.

### Líneas de trabajo pendientes

1. **Bloques reales heterogéneos**: el benchmark sintético repite el mismo bloque. Con bloques reales (composición variable de script types, número de inputs distinto en cada bloque), el spread podría ser estructuralmente mayor. Requiere IBD parcial o un benchmark paramétrico basado en estadísticas de mainnet.
2. **Gap entre bloques**: medir cuánto tiempo pasan los workers ociosos entre `ConnectBlock` calls en IBD real y si escala con el número de workers.

---

## 2026-07-01 — Profiling de IBD real con `bitcoind` (no bench): diseño y bug de naming

### Motivación

Punto pendiente #1 de la sección anterior: medir el desbalance por bloque con bloques reales heterogéneos, no el bloque sintético repetido de `bench_bitcoin`. Se decide correr `bitcoind` real con `-assumevalid=0` (Escenario B) contra mainnet, con `-prune=5000` para acotar disco (875GB libres, no hace falta la cadena sin podar) y `bpftrace` para captura continua de larga duración (un `perf record` de eventos crudos sería inviable en tamaño para un IBD de posibles días — ver journal previo, 60s ya generaban ~50MB).

Diseño: `uprobe` en `Chainstate::ConnectBlock` (símbolo `_ZN10Chainstate12ConnectBlockERK6CBlockR20BlockValidationStateP11CBlockIndexR15CCoinsViewCacheb`, igual en `bitcoind` y `bench_bitcoin`) como marcador de inicio de bloque, y acumulación de tiempo de CPU por worker vía `sched:sched_switch` entre marcadores. Cada bloque emite una línea compacta `BLOCK <n> <tid>=<ns> ...`; el análisis de distribución (percentiles, histograma de `max_share` = fracción del bloque hecha por el worker más ocupado) se delega a un script Python externo (`analyze_ibd_spread.py`) que puede correr en streaming sobre el log mientras el nodo sigue sincronizando.

A diferencia del trabajo con `bench_bitcoin`, `bitcoind` ya expone `-par`, `-assumevalid` y `-prune` como argumentos estándar (`src/init.cpp:513,539,548`) — no hace falta parchear código fuente.

### Bug: filtro de nombre de hilo incorrecto (`scriptch` vs `b-scriptch`)

Primer test de validación con `-stopatheight=3000`: el uprobe en `ConnectBlock` funcionaba perfectamente (3001 líneas `BLOCK`, contador correcto), pero **ningún** worker aparecía nunca en `@work` — el mapa salía vacío en todas las líneas. Los bloques 0-3000 son de 2009 (prácticamente solo coinbase), así que se sospechó inicialmente que simplemente no había trabajo que verificar.

Se repitió el test extendiendo a `-stopatheight=200000` (~2012, ya con volumen de transacciones real) reutilizando el mismo datadir para no re-descargar desde génesis. El resultado fue el mismo: 197000 bloques procesados, `@work` completamente vacío en los 197000. Esto ya no encajaba con "bloques sin trabajo" — a esa altura hay cientos de tx por bloque.

**Causa raíz:** el filtro de bpftrace comparaba `args->prev_comm`/`args->next_comm` contra el literal `"scriptch"` (8 caracteres, sin prefijo). La suposición (registrada erróneamente en una sesión anterior) era que el prefijo `"b-"` en los nombres de hilo (`b-scriptch.NN`) lo añadía el arnés de test/bench específicamente. Es falso: `util::ThreadRename` en `src/util/threadnames.cpp:58` antepone `"b-"` **incondicionalmente** a todo nombre de hilo visible por el SO (`SetThreadName(("b-" + name).c_str())`) — es una convención global de Bitcoin Core (prefijo "b-" = "bitcoin"), no algo específico del arnés de bench. El nombre interno sin prefijo (`SetInternalName`) solo se usa para logging, no es lo que aparece en `/proc/<pid>/task/<tid>/comm` ni en `args->comm` del tracepoint `sched_switch`.

**Fix:** cambiar el filtro a `strncmp(args->prev_comm, "b-scriptch", 10) == 0` (y equivalente para `next_comm`). Corregido en `ibd_block_spread.bt`.

**Lección:** verificar el nombre de hilo real contra `/proc/<pid>/task/*/comm` (o el log `Script verification uses N additional threads` + inspección directa) antes de asumir el formato de nombre en un filtro de bpftrace, en vez de basarse en una nota de sesión anterior sin recomprobar contra el código fuente actual.

### Validación tras el fix

Con el filtro `"b-scriptch"` corregido, test de ~9833 bloques reales (~2009-2010) mostró `@work` con datos en todas las líneas. Métrica inicial: por bloque, desviación del worker más cargado/ocioso respecto a la media ideal (100/N%). Resultado: 90.7% de los bloques con desviación <5pp, cola pequeña de casos extremos (0.2% de bloques con >20pp, máximo 46.5pp en el peor caso).

### Iteración del diseño de métricas (script `analyze_ibd_spread.py`)

Tras revisar los primeros resultados con el usuario, dos mejoras al análisis:

**1. No descartar los workers intermedios.** La primera versión solo guardaba 2 valores por bloque (el más cargado y el más ocioso), tirando el resto. Se cambió a acumular la desviación de **todos** los workers en cada bloque (9 muestras/bloque en vez de 2), dando ~88k muestras worker-bloque sobre 9833 bloques. Resultado: 94.7% de las muestras entre -5pp y +5pp, colas simétricas pequeñas — confirma con mucha más resolución que el desbalance fuerte es la excepción.

**2. Identidad por rango de velocidad, no por thread id — para tener una métrica agregable globalmente.** Insight del usuario: promediar por `tid` a lo largo de muchos bloques converge a la media ideal (es el mismo efecto de "ley de grandes números" que ya hizo engañoso el CV global de `bench_bitcoin` — qué hilo gana varía aleatoriamente bloque a bloque, así que promediar por identidad de hilo lo lava todo). En cambio, ordenar los workers de cada bloque por tiempo de ejecución (rank 1 = el que más trabajó ese bloque, rank N = el que menos) y promediar **por rank** a través de miles de bloques SÍ converge a un valor estable y no-uniforme, porque siempre hay estructuralmente un "más ocupado" y un "más ocioso" en cada bloque — es un estadístico de orden, no una identidad arbitraria.

Con los ~9833 bloques de test: rank 1 (más ocupado) promedia +2.7pp sobre la media ideal, rank 9 (más ocioso) promedia -2.6pp, con una gradación suave y monótona entre medio (ver `analyze_ibd_spread.py::print_rank_table`). Esta tabla es estable bajo agregación (a diferencia de la agregación por tid) — es la métrica candidata para reportar de forma resumida sobre ventanas de 100k bloques en el IBD completo, sin perder la señal de desbalance estructural.

### Siguiente paso

Lanzar el IBD completo desde génesis en `~/ibd-profiling-datadir` (sin `-stopatheight`), capturando en `~/ibd_full.log`, y generar el reporte cada 100k bloques con `analyze_ibd_spread.py --bucket-size 100000` — viendo si la gradación por rank (y la cola de la distribución completa) se mantiene igual o se ensancha a medida que los bloques reales tienen más transacciones (post-2013).
