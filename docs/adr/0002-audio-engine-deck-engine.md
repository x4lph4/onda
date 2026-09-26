# ADR-0002 — Motor de audio + deck engine (Fase 2)

Estado: aceptado (26 Sep 2026)

## Contexto

F3 demostró la tubería decode→fifo→deck→mixer con un `AudioFileStreamer`
por deck (streaming desde disco). La Fase 2 exige seek/loop/reverse/
keylock deterministas y calidad de directo; el modelo de streaming hacía
todo eso frágil (latencia de seek, underruns, complejidad RT).

## Decisiones

### 1. Tracks completamente decodificados en RAM (`TrackAudio`)

Cada deck reproduce desde un `std::vector<float>` interleaved estéreo
decodificado a la *sample rate del dispositivo* en un hilo worker.

- Seek/loop/reverse/pitch = aritmética de índices: O(1), cero I/O,
  cero latencia adicional, determinista.
- ~95 MB por track de 4 min a 48 kHz — aceptable para 4 decks (~400 MB).
- El audio callback jamás espera a disco (requisito duro).
- Handoff loader→RT: dos slots atómicos `pendingTrack_`/`retiredTrack_`.
  El deck adopta al inicio del bloque; retira el anterior para que el
  loader lo libere. Propiedad única: el loader es dueño de la memoria
  (libera en su propio hilo, nunca en RT).

Alternativa descartada: streaming con ring buffer (latencia de seek,
casos límite de EOF/loop/reverse mucho más complejos).

### 2. Reloj musical separado del cabezal de lectura (`dispPos_`)

En modo key-lock el cabezal `pos_` corre por delante dentro del
stretcher y no es la posición sonora. El transporte mantiene un reloj
propio que avanza `|rate|` por frame de salida y aplica el loop a nivel
musical: es la posición que ve la UI y la que consumirán sync/quantize.
Determinista, estable con loops bajo cualquier pitch.

### 3. Key lock mediante `ITimeStretcher` + WSOLA propio

`WsolaStretcher` (grano W=2048, hop Hs=W/2, alineación por correlación
±W/8, buffers circulares preasignados, cero allocations RT). DeckEngine
solo conoce la interfaz — reemplazable por Rubber Band/SoundTouch sin
tocar el deck.

### 4. Cadena de ganancia

`trackGain → deckGain → EQ 3 bandas → DJ filter → [mixer] fader →
crossfader (curva) → master → limiter opcional → out`.
Buses de cue independientes (PFL pre-fader, cueVol, cueMix, salidas 2-3).

### 5. Curvas de crossfader

`Linear`, `EqualPower` (cos/sin) y `Cut` (unidad hasta ±0.85, caída al
rail). `xfGain(curve, x, sideA)` — nuevas curvas = un enum + una rama.

### 6. Suavizado de clicks

Toda discontinuidad de transporte (seek, loop wrap, keylock on/off,
reverse, adopt) dispara un fade-in de ~5.3 ms (1/256 por muestra).
Ganancia de deck suavizada con τ ≈ 20 ms. Las curvas/EQ evitan zipper
recomputando coeficientes solo en cambio de parámetro.

### 7. Errores

TrackLoader publica `state=Error` + descripción sin excepciones hacia
RT/UI. Device down → `audioDeviceStopped` → log + flag, la app sigue.
EOF → `Paused` + `streamEnded`. Underruns de stretcher se contabilizan
en `DeckStats::underruns`.

### 8. Métricas RT

`AudioStats`: duración/max de callback, xruns (gap > 1.75 periodos),
CPU suavizada, sample rate/buffer, clip count. `DeckStats`: estado,
posición, niveles, underruns, progreso/latencia de carga. `MasterStats`:
peak/RMS por canal, ganancia del limiter, cue disponible, clips.

## Consecuencias

- La UI solo envía `rt::Command` por cola SPSC — nunca toca DSP.
- 4 decks = array `MixerEngine::kMaxDecks`; la UI expone 2.
- Sync/quantize de verdad requerirá beatgrid (fase de análisis);
  `setQuantize` ya viaja por comando y queda publicado en DeckStats.
- El estrés simulado (~110 s de audio, 2 decks + loops + keylock +
  crossfader + cue) corre en ~10 s en tests con 0 allocations RT.
