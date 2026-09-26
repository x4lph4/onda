# ADR-0004 — Performance Engine (Fase 4)

Estado: aceptado (26 Sep 2026)

## Contexto

Fase 4 convierte los decks en instrumentos de performance: Sync,
Quantize, Hot/Memory/Loop Cues, Beat Jump, Smart Loops, Loop Roll y
Slip — sin tocar el modelo RT de la Fase 2 ni depender de hardware o UI.

## Decisiones

### 1. Dos mitades estrictas

**RT (audio callback)** — solo corrección continua, matemática pura
sobre estado propio:

- `SyncRtCore` vive en `AudioEngine`; corre *después* del drain de
  comandos y *antes* del render, calculando por bloque la corrección
  de cada follower contra el master.
- `DeckEngine` gana: `grid_` (handoff atómico `pendingGrid_/retiredGrid_`,
  mismo protocolo que `pendingTrack_`), `seekFrame`, slip shadow
  (`slipShadow_` avanza a `|rate|` mientras el audio hace otra cosa) y
  la máquina de corrección de sync.

**Non-RT (UI/controller thread)** — `PerformanceEngine` facade que
traduce acciones musicales a comandos de la cola. Los sub-motores
(`CueEngine`, `SmartLoopEngine`, `BeatJumpEngine`, `QuantizeEngine`)
son **lógica pura** — devuelven frames, nunca tocan audio ni locks.
Testeables al 100 % sin dispositivo: los tests inyectan un sink que
aplica `applyDeckCommand` directamente — *el mismo* código que corre
el callback (`core/engine/DeckCommand.h`, compartido).

### 2. Sync: fase predecida un bloque adelante

`SyncRtCore::apply` predice `phaseAfter(blockFrames)` del master
*antes* de renderizar — el orden de render de los decks deja de
importar. Follower:

- **Tempo (Bpm/Beat/Phrase)**: rate total = `masterBpm/ownBpm`,
  slew-limitado a ±0.2 %/bloque (sin clicks); el pitch del usuario se
  ignora mientras sync está activo (documentado); respeta `pitchRange_`
  — fuera de rango → `syncLimited` → `SyncStatus::OutOfRange`.
- **Fase (≥Beat)**: error envuelto a ±0.5 beats. Deadband ~0.17 ms
  (anti-jitter); chase acotado a 5 % de rate/bloque; error > 0.35 beats
  → **un snap con fade** en vez de una persecución audible.
- Master parado o sin grid fiable → followers mantienen el último
  tempo convergido (determinista, sin correcciones).
- `SyncMode::Phrase` reutiliza el lock de fase; el *alineado* por
  frases/compases es responsabilidad del snap no-RT (la detección de
  frases entrará con análisis estructural futuro).

### 3. Elección de master determinista

El primer deck en activar sync se convierte en master
(`enableSync` → `setMasterDeck`). `setMasterDeck` cambia dinámicamente.
`syncStatus` reporta estados explícitos — `NoGrid`, `NoGridMaster`,
`OutOfRange`, `UnreliableMaster`, `DeckStopped` — y `lastError`
recuerda peticiones rechazadas. Nada se esconde.

### 4. Quantize = snapping de objetivo, no espera

Per spec: quantize **no** espera al próximo beat — recalcula la posición
destino sobre la grid (`QuantizeEngine::snap`, resolución 1/1…1/16)
y ejecuta el seek con fade anti-click inmediato. Aplica a cues, loop
in/out, beat jump y slip actions.

### 5. Slip como shadow clock en el deck

`beginSlip` congela `slipShadow_` en la posición musical; avanza a
`|rate|` por bloque mientras el audio real hace roll/cue/jump.
`endSlip` = faded seek a la shadow. Hot cue hold, roll, reverse —
todos reutilizan la misma primitiva; el deck nunca pierde la referencia.

### 6. Persistencia (migración v2)

`cues(track_id, slot, type, frame, frame2, beat, beat2, argb, label)` —
guarda **frame y beat**: tras una edición manual del beatgrid, el cue
re-resuelve su posición musical. `saved_loops` igual. `kv` para
settings de quantize. Todo write-through fuera del hilo RT; los datos
pertenecen al track, no a la sesión.

### 7. Conflictos: orden de llegada en una sola cola

Todas las acciones son comandos en una SPSC FIFO → serializados.
Prioridades documentadas: transport > seek-family > loop > sync
(la corrección de sync jamás pisa transporte: solo ajusta rate/posición
dentro de sus límites). Un seek/cue/jump durante roll aborta el roll
restaurando la shadow (`exitLoop`/`loopRollEnd`).

### 8. Eventos

`PerfEvent` por acciones no-RT (cue/sync/loop/jump/slip/quantize/master)
más `poll()` (tick de UI ~30 Hz) que detecta transiciones
`BeatChanged`/`BarChanged`/`LoopEntered`/`LoopExited` leyendo
`DeckStats` — los mismos canales que consumirá ControllerEngine/MIDI.

## Interacción Pitch × Sync × KeyLock (documentada)

| Estado | Comportamiento |
|---|---|
| KeyLock ON + Sync | tempo cambia vía `syncMul`, pitch musical fijo (WSOLA) |
| KeyLock OFF + Sync | tempo y pitch se mueven juntos (varispeed) |
| Sync ON | pitch fader ignorado; `pitchRange` limita el sync |
| Sync OFF | pitch/bend normales |
| Bend | siempre aditivo, incluso sincronizado |

## Consecuencias

- `DeckStats` ahora publica `bpm/beatPos/barPos/effBpm/syncMode/
  syncErrMs/syncLimited/slip` — base del MasterClock no-RT y de la UI.
- 4 decks soportados estructuralmente (tests usan 3 simultáneos).
- UI: botones SYNC/QNT por deck; el grid se adjunta al terminar el
  análisis. Sin UI de cues/waveform aún (fase UI posterior).
- La corrección de sync es por diseño *suave*: converge en segundos,
  no instantáneamente — igual que el equipo DJ real.
