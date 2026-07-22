# Backlog: fork maintenance and modular patches

Цель: держать форк с кастомными фичами, периодически подтягивая `main` (upstream), с **минимальными конфликтами** при merge.

Текущее состояние (`main` → `my_patches`, ориентир на июль 2026):
- ~4900 строк diff в 22 файлах.
- Уже вынесено в отдельные модули: `session/`, `stream/`, `util/MediaUri`, большая часть `mpris/`.
- Основной долг: `GtkPlayerWindow.cpp` (+~2400 строк), затем `ExternalAudioDecoder.cpp` (+~360), `M3uPlaylistReader.cpp` (+~200).

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

- [ ] Завести каталоги `include/pcmtp/patches/` и `src/patches/`.
- [ ] Любая новая фича — новые `.hpp`/`.cpp` в `patches/`, не раздувание `GtkPlayerWindow`.
- [ ] В апстримных файлах — только: include, поле-менеджер, 1–3 вызова в lifecycle (init/play/stop/shutdown).
- [ ] Документировать glue в комментарии `// PATCH: <feature>` для быстрого поиска при merge.

### Фаза 1 — вынести стримы из GtkPlayerWindow (высокий приоритет)

**Новый класс:** `StreamPlaybackManager` (`patches/StreamPlaybackManager.hpp/.cpp`)

Забирает из `GtkPlayerWindow`:
- async stream probe (queue, worker thread, generation)
- reconnect scheduling
- `StreamSidecar` / ICY now-playing
- `StreamHealthRegistry` integration
- `note_stream_broken`, health UI refresh hooks

**Glue в GtkPlayerWindow:** поле `stream_manager_`, вызовы из `play_track_index_at_offset`, `stop_playback`, timer tick.

**Оценка:** −800…1000 строк из `GtkPlayerWindow.cpp`.

### Фаза 2 — сессия плейлиста

**Новый класс:** `PlaylistSessionController` (`patches/PlaylistSessionController.hpp/.cpp`)

Забирает:
- `save_playlist_session` / `restore_playlist_session`
- `session_track_from_entry` / `entry_from_session_track` / `session_track_restorable`
- restore focus idle

`PlaylistSession` (уже отдельный) остаётся как storage; контроллер — оркестрация.

**Оценка:** −350…450 строк.

### Фаза 3 — MPRIS bridge

**Новый класс:** `MprisPlayerBridge` (`patches/MprisPlayerBridge.hpp/.cpp`)

Забирает из `GtkPlayerWindow`:
- `setup_mpris`, все `mpris_*`, `build_mpris_state`, cover cache
- `notify_mpris_state_changed`, `mark_mpris_track_changed`

`MprisService` остаётся низкоуровневым D-Bus слоем (можно оставить общим с upstream).

**Оценка:** −450…550 строк.

### Фаза 4 — декодер потоков

**Новый класс:** `StreamAudioDecoder` : `IAudioDecoder` (`patches/StreamAudioDecoder.hpp/.cpp`)

Перенести из `ExternalAudioDecoder`:
- `is_stream_uri` / stream ffprobe / reconnect ffmpeg flags / `verify_stream_playback`

В фабрике декодеров (glue):
```cpp
if (is_stream_uri(path)) return std::make_unique<StreamAudioDecoder>(...);
```

**Оценка:** апстримный `ExternalAudioDecoder` → diff к `main` близок к нулю.

### Фаза 5 — M3U radio / EXTINF

**Новый модуль:** `M3uRadioPlaylistReader` или расширение в `patches/M3uPlaylistExtensions.cpp`

- EXTINF, remote URI, `MediaUri` resolution
- Базовый `M3uPlaylistReader` в upstream не трогать (или только добавить virtual hook `parse_line`).

**Оценка:** −150…200 строк из апстримного reader.

### Фаза 6 — модель плейлиста

Заменить раздувание `PlaylistEntry` композицией:
```cpp
struct PlaylistEntry { /* upstream core */ };
struct PatchTrackExtensions {
    bool is_stream = false;
    bool stream_format_probed = false;
    std::uint32_t source_bit_rate = 0;
    // ...
};
// или std::variant<FileTrack, StreamTrack, CueTrack>
```

Цель: при merge upstream структура трека не конфликтует каждый раз.

### Фаза 7 — опциональная static lib

```
add_library(pcm_transport_patches STATIC src/patches/...)
target_link_libraries(pcm_transport PRIVATE pcm_transport_patches)
```

`CMakeLists.txt` upstream: +2 строки. Все патчи линкуются одним блоком.

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
- [ ] Все stream/session/mpris-специфичные unit-зоны в `src/patches/`.
- [ ] Документирован ритуал merge upstream (см. выше).
- [ ] Cursor rule `.cursor/rules/fork-patch-workflow.mdc` соблюдается в новых PR/коммитах.

---

## Не в scope рефакторинга (осознанно)

- Полный отказ от правок `GtkPlayerWindow` — UI всё равно собирается там.
- Вынос GTK widget tree в отдельный builder — отдельный большой проект.
- Автоматический merge без ручной проверки MPRIS/stream/session.

---

## История

- 2026-07-22: создан backlog после анализа diff `main` vs `my_patches` (интернет-радио, session, MPRIS, media keys, Path format).
