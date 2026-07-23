# Backlog: fork maintenance and modular patches

Цель: держать форк с кастомными фичами, периодически подтягивая `main` (upstream), с **минимальными конфликтами** при merge.

Текущее состояние (после merge upstream 0.9.110, июль 2026):
- `main` = upstream `3011cae` (MPRIS, MediaProbe, DSD, desktop integration).
- `my_patches` = merge upstream + форковые фичи (стримы, session, playlist search).
- MPRIS **в апстриме** (~400 строк в `GtkPlayerWindow`); форк добавляет ~30–50 строк патчей.

Целевое состояние: апстримные файлы меняются **тонко** (glue, hooks, factory); логика фич — в `include/pcmtp/patches/` + `src/patches/`.

---

## Git-модель веток

| Ветка | Назначение |
|-------|------------|
| `main` | Зеркало upstream. Только `fetch`/`merge`/`rebase` от origin upstream, без кастомных фич. |
| `fixes` | Мелкие багфиксы, потенциально пригодные для PR в upstream. База: `main`. |
| `my_patches` | Интеграционная ветка форка: все кастомные фичи + merge из `fixes`. |

### Ритуал подтягивания upstream

1. `git fetch upstream` (remote на оригинальный репозиторий).
2. Обновить `main`: `git checkout main && git merge upstream/main` (или rebase, если договорённость одна).
3. `git checkout fixes && git rebase main` — разрешить конфликты в общих файлах.
4. `git checkout my_patches && git merge fixes` — затем `git merge main` (или rebase `my_patches` на `main` после merge fixes).
5. Сборка и smoke-тест (открыть файл, play/pause, stream, session restore, MPRIS).
6. Пуш своих веток на `origin`.

### Куда коммитить новую работу

- Багфикс без привязки к фиче форка → `fixes` → merge в `my_patches`.
- Новая фича форка → отдельная ветка от `my_patches` (или `fixes`), затем merge в `my_patches`.
- Не коммитить кастомные фичи напрямую в `main`.

---

## Архитектурный план рефакторинга

### Фаза 0 — правила для нового кода (сразу, без большого рефакторинга)

- [x] Завести каталоги `include/pcmtp/patches/` и `src/patches/`.
- [x] Любая новая фича — новые `.hpp`/`.cpp` в `patches/`, не раздувание `GtkPlayerWindow`.
- [x] В апстримных файлах — только: include, поле-менеджер, 1–3 вызова в lifecycle (init/play/stop/shutdown).
- [x] Документировать glue в комментарии `// PATCH: <feature>` для быстрого поиска при merge.

### Фаза 1 — вынести стримы из GtkPlayerWindow (высокий приоритет)

**Новый класс:** `StreamPlaybackManager` (`patches/StreamPlaybackManager.hpp/.cpp`)

Забирает из `GtkPlayerWindow`:
- async stream probe (queue, worker thread, generation)
- reconnect scheduling
- `StreamSidecar` / ICY now-playing
- `StreamHealthRegistry` integration
- `note_stream_broken`, health UI refresh hooks

**Glue в GtkPlayerWindow:** поле `stream_manager_`, вызовы из `play_track_index_at_offset`, `stop_playback`, timer tick.

**Статус (2026-07-23):** реализовано. Дополнительно: `StreamPlaylistUtils` (метки/эвристики из M3U).

### Фаза 2 — сессия плейлиста

**Новый класс:** `PlaylistSessionController` (`patches/PlaylistSessionController.hpp/.cpp`)

Забирает:
- `save_playlist_session` / `restore_playlist_session`
- `session_track_from_entry` / `entry_from_session_track` / `session_track_restorable`
- restore focus idle

`PlaylistSession` (уже отдельный) остаётся как storage; контроллер — оркестрация.

**Статус (2026-07-23):** реализовано. `PlaylistSessionEntryData` — DTO для glue с `GtkPlayerWindow::PlaylistEntry`.

### Фаза 3 — MPRIS: минимальные патчи (пересмотрено 2026-07-23)

**Решение:** MPRIS принят в upstream (`GtkPlayerWindow` + `MprisService`). Вынос в `MprisPlayerBridge` **отменён** — конфликтует с апстримом.

**Форковые патчи** (держать в `GtkPlayerWindow` с `// PATCH: stream-mpris`):

| Патч | Где |
|------|-----|
| `update_playlist_selection_from_ui()` в play/next/prev | `setup_mpris` |
| ICY title в `build_mpris_state` | `stream_manager_->now_playing()` |
| URL/cover для стримов | `build_mpris_state` |
| `validate_mpris_open_uri` + http/icy | `mpris_open_uri` |
| Media keys (`handle_media_key`) | `GtkPlayerWindow` |

**Форковые патчи в `MprisService.cpp`** (кандидат на PR в upstream):

- `dispatch_on_main_context` — D-Bus → GTK main thread
- `SupportedUriSchemes`: `file, http, https`

**Статус:** патчи сохранены при merge upstream 0.9.110.

### Фаза 4 — декодер потоков

**Новый класс:** `StreamAudioDecoder` : `IAudioDecoder` (`patches/StreamAudioDecoder.hpp/.cpp`)

Перенести из `ExternalAudioDecoder`:
- `is_stream_uri` / stream ffprobe / reconnect ffmpeg flags / `verify_stream_playback`

В фабрике декодеров (glue):
```cpp
if (is_stream_uri(path)) return std::make_unique<StreamAudioDecoder>(...);
```

**Статус:** реализовано — `StreamAudioDecoder` в `patches/`, stream-логика вынесена из `ExternalAudioDecoder`.

### Фаза 5 — M3U radio / EXTINF

**Новый модуль:** `M3uPlaylistExtensions` (`patches/M3uPlaylistExtensions.hpp/.cpp`)

- EXTINF, remote URI fetch (curl), `MediaUri` resolution, remote `.m3u` detection
- `M3uPlaylistReader::read_entries` → делегирует в extensions
- `M3uPlaylistReader::read_local_paths` — upstream-стиль (локальные пути, без remote)
- `GtkPlayerWindow` импортирует плейлисты через `read_entries` (стримы + EXTINF)

**Статус (2026-07-23):** реализовано. `M3uPlaylistReader.cpp` ~180 строк, близко к upstream.

**Оценка:** −150…200 строк из апстримного reader.

### Фаза 6 — модель плейлиста

`PatchTrackExtensions` (`patches/PatchTrackExtensions.hpp`) — композиция в `GtkPlayerWindow::PlaylistEntry`:

```cpp
struct PlaylistEntry { /* upstream core fields */ };
PatchTrackExtensions patch{};  // is_stream, stream_format_probed, source_bit_rate
```

**Статус (2026-07-23):** реализовано. Форковые поля вынесены в `entry.patch.*`.

### Фаза 7 — static lib

```cmake
add_library(pcm_transport_patches STATIC ...)
target_link_libraries(pcm_transport PRIVATE pcm_transport_patches)
```

**Статус (2026-07-23):** реализовано. Все `src/patches/*.cpp` линкуются через `pcm_transport_patches`.

---

## Файлы: что трогать при merge чаще всего

| Файл | Сейчас | Цель после рефакторинга |
|------|--------|-------------------------|
| `GtkPlayerWindow.cpp` | очень большой diff | glue ~100–200 строк |
| `GtkPlayerWindow.hpp` | +40 методов | 3–5 полей-менеджеров |
| `ExternalAudioDecoder.cpp` | stream logic внутри | только файлы |
| `M3uPlaylistReader.cpp` | EXTINF внутри | отдельный reader |
| `MprisService.cpp` | улучшения D-Bus | общий с upstream или `patches/` |
| `CMakeLists.txt` | +5 файлов | +1 строка на lib или блок `patches/` |

---

## Метрики готовности (Definition of Done для рефакторинга)

- [ ] `git diff main...my_patches -- GtkPlayerWindow.cpp` < 400 строк изменений.
- [x] Все stream/session/mpris-специфичные unit-зоны в `src/patches/`.
- [x] Документирован ритуал merge upstream (см. выше).
- [x] Cursor rule `.cursor/rules/fork-patch-workflow.mdc` соблюдается в новых PR/коммитах.
- [x] `pcm_transport_patches` static lib собирает все патчи.

---

## Не в scope рефакторинга (осознанно)

- Полный отказ от правок `GtkPlayerWindow` — UI всё равно собирается там.
- Вынос GTK widget tree в отдельный builder — отдельный большой проект.
- Автоматический merge без ручной проверки MPRIS/stream/session.

---

## История

- 2026-07-22: создан backlog после анализа diff `main` vs `my_patches` (интернет-радио, session, MPRIS, media keys, Path format).
