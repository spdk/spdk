# WAL bdev: назначение и API

Этот документ описывает виртуальное устройство `wal` из `module/bdev/wal`: как оно устроено, какие RPC доступны и как пользователю запускать и восстанавливать его.

## Что делает WAL

- WAL объединяет два базовых диска: `main` (рабочие данные) и `journal` (журнал).
- Каждая запись проходит путь: запись в журнал → `flush` журнала → запись в основной диск → `flush` основного диска. Ответ пользователю возвращается только после сброса обеих копий.
- Во время восстановления чтения, попадающие в ещё не восстановленный диапазон, обслуживаются из журнала; записи на время восстановления отвергаются.
- Восстановление (`wal_bdev_recover`) сравнивает содержимое журнала и основного диска чанками по 1024 блока и копирует только отличающиеся блоки.

## RPC-интерфейс

RPC можно отправлять через `scripts/rpc.py` или напрямую через socat на `/var/tmp/spdk.sock`.

### wal_bdev_create

Создаёт WAL-диск поверх двух существующих bdev.

Параметры:

- `name` — экспортируемое имя WAL-диска.
- `journal_name` — имя bdev для журнала.
- `bdev_name` — имя основного bdev.
- `block_size` (опционально) — не настраивает геометрию, значение берётся из базовых дисков; параметр удобен только для читаемости.
- `size_mb` (опционально) — аналогично, итоговый размер совпадает с основным bdev.

Условия: блок‑размеры базовых bdev должны совпадать; журнал и основной диск должны быть разными устройствами. Размер WAL равен размеру основного диска.

Пример:

```sh
scripts/rpc.py wal_bdev_create \
  --name wal0 \
  --journal-name aio_journal \
  --bdev-name aio_main \
  --block-size 4096
```

Ответ: строка с именем созданного устройства. Геометрию можно посмотреть через `scripts/rpc.py bdev_get_bdevs -b wal0`.

### wal_bdev_delete

Удаляет WAL-диск. Если в момент вызова идёт восстановление, удаление ставится в очередь: восстановление будет прервано, после чего устройство дерегистрируется.

```sh
scripts/rpc.py wal_bdev_delete wal0
```

### wal_bdev_recover

Запускает восстановление (журнал → основной диск) для указанного WAL-диска. Команда асинхронная: RPC завершается после окончания копирования или при ошибке.

```sh
scripts/rpc.py wal_bdev_recover wal0
```

## Документация по API

1. Запустить `spdk_tgt` (для локального теста удобно с AIO-файлами):
   ```sh
   sudo rm -f /var/tmp/spdk.sock /dev/shm/spdk* /var/run/.spdk* /var/run/.rte_config
   sudo env SPDK_APP_DPDK_ARGS="--file-prefix=waltest --lcores=0" \
     LD_LIBRARY_PATH=$(pwd)/build/lib ./build/bin/spdk_tgt --wait-for-rpc
   ```
2. Создать базовые диски (пример на AIO):
   ```sh
   dd if=/dev/zero of=/tmp/main.img bs=1M count=128
   dd if=/dev/zero of=/tmp/journal.img bs=1M count=128
   scripts/rpc.py bdev_aio_create /tmp/main.img aio_main 4096
   scripts/rpc.py bdev_aio_create /tmp/journal.img aio_journal 4096
   ```
3. Создать WAL и пользоваться им как обычным bdev:
   ```sh
   scripts/rpc.py wal_bdev_create -n wal0 -b aio_main -j aio_journal
   scripts/rpc.py bdev_get_bdevs -b wal0
   ```
   Далее можно подключить `wal0` к bdevperf, vhost, nvmf, lvol и т.п.
4. Эмулировать сбой/расхождение и восстановить:
   ```sh
   # остановить spdk_tgt, испортить основной диск, оставить журнал нетронутым/правильным
   scripts/rpc.py wal_bdev_recover wal0
   ```
   Детальный пошаговый сценарий приведён в `WAL_recovery_demo.md`.

## RPC
Ниже приведён минимальный сценарий, позволяющий создать WAL-устройство и убедиться, что RPC и регистрация bdev работают корректно.

### Предварительные условия

- SPDK собран: `./configure && ninja -C build`
- Выделены hugepages и настроено окружение DPDK:
  - `sudo HUGEMEM=1024 scripts/setup.sh config`
  - `scripts/setup.sh status`

### Запуск `spdk_tgt`

В одном терминале:

```sh
sudo rm -f /var/tmp/spdk.sock /dev/shm/spdk* /var/run/.spdk* /var/run/.rte_config
sudo env SPDK_APP_DPDK_ARGS="--file-prefix=waltest --lcores=0" \
  LD_LIBRARY_PATH=$(pwd)/build/lib \
  ./build/bin/spdk_tgt --wait-for-rpc
```

Во втором терминале (однократно после старта, если используется `--wait-for-rpc`):

```sh
sudo scripts/rpc.py framework_start_init
sudo scripts/rpc.py framework_wait_init
```

### Создание базовых bdev (пример на AIO)

Подготовить два файла одинакового размера (пример — 128 MiB):

```sh
dd if=/dev/zero of=/tmp/main.img bs=1M count=128
dd if=/dev/zero of=/tmp/journal.img bs=1M count=128
```

Создать два AIO bdev с одинаковым `block_size`:

```sh
sudo scripts/rpc.py bdev_aio_create /tmp/main.img aio_main 4096
sudo scripts/rpc.py bdev_aio_create /tmp/journal.img aio_journal 4096
```

Проверить, что устройства зарегистрированы:

```sh
sudo scripts/rpc.py bdev_get_bdevs -b aio_main
sudo scripts/rpc.py bdev_get_bdevs -b aio_journal
```

### Создание WAL-устройства

```sh
sudo scripts/rpc.py wal_bdev_create -n wal0 -b aio_main -j aio_journal --block-size 4096
sudo scripts/rpc.py bdev_get_bdevs -b wal0
```
## Семантика ввода-вывода

- Поддерживаемые операции: READ, WRITE, FLUSH, RESET. Остальные типы IO отклоняются.
- WRITE и FLUSH запрещаются во время восстановления (`SPDK_BDEV_IO_STATUS_FAILED`), чтобы не смешивать новые и восстанавливаемые данные.
- RESET транслируется в основной диск.
- Требования по выравниванию берутся как максимум между align обоих базовых дисков.
- Восстановление работает чанками по 1024 блока; при совпадении данных переписывание не выполняется.

## Связанные материалы

- `WAL_recovery_demo.md` — воспроизводимый сценарий падения и восстановления.
- `WAL_recovery_usage.md` — тот же сценарий с командами `scripts/rpc.py`.
- Код модуля: `module/bdev/wal/vbdev_wal.c`, RPC: `module/bdev/wal/bdev_wal_rpc.c`.
