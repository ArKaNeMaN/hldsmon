# Vendored headers

Заголовки вендорены из внешних репозиториев (для сборки плагина). Точные коммиты:

| Источник | Commit | Что скопировано |
|---|---|---|
| `rehlds/ReAPI` | `95089cb0bcc1d022f74b23e1b7250c3f8ba26a8b` | `include/cssdk/` (HLSDK public/dlls/engine/game_shared/pm_shared/common), `include/metamod/` (Metamod SDK), `include/com_client.h`, `com_net.h`, `com_progdefs.h`, `com_delta_packet.h` |
| `rehlds/ReHLDS` | `550f2d62f13f4ebeb029c1d9d1c212133202611d` | (проверочный источник; структуры `client_t`/`netchan_t` приходят из `com_client.h`/`com_net.h` ReAPI) |
| `rehlds/Metamod-R` | `56590c5c70fa95775051888b7fe0a4e285856ebb` | (проверочный источник; Metamod SDK берётся из ReAPI `include/metamod`) |

**Ключевые пути:**
- `include/cssdk/engine/rehlds_api.h` — `IRehldsApi`, `IRehldsHookchains`.
- `include/cssdk/engine/rehlds_interfaces.h` — `IGameClient`, `INetChan`, `IRehldsServerStatic`. (Версии `GetClient`/`GetChan` — под `REHLDS_SELF`; НЕ определяем, используем `GetClient_t`.)
- `include/com_client.h` — полный `client_t` (+ `clientOfIndex`).
- `include/com_net.h` — `netchan_t`.
- `include/metamod/meta_api.h` — Metamod ABI.

**Не редактировать** вендоренные файлы. Лицензии — GPL/Valve SDK (внутренний инструмент).
