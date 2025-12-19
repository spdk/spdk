# Как пользоваться сценарием WAL_recovery_demo

Этот файл поясняет, как запускать и воспроизводить сценарий из `WAL_recovery_demo.md` с учётом актуальных команд `scripts/rpc.py`. Краткий end-to-end сценарий (создание WAL + проверка recovery) приведён в `wal_demo.md` (раздел «Тестирование восстановления»).

## Что проверяем
- WAL-модуль пишет данные зеркально: сначала в журнал, затем в основной диск.
- После аварийного завершения `spdk_tgt` и расхождения содержимого журнал → основной диск восстанавливается командой `wal_bdev_recover`.

## Предварительные условия
- SPDK собран: `./configure && ninja -C build`.
- Выделены hugepages: `sudo HUGEMEM=1024 scripts/setup.sh config` и проверка `scripts/setup.sh status`.
- Созданы два файла-образа по 128 MiB:
  ```sh
  dd if=/dev/zero of=/tmp/main.img bs=1M count=128
  dd if=/dev/zero of=/tmp/journal.img bs=1M count=128
  ```

## Запуск spdk_tgt
```sh
sudo env LD_LIBRARY_PATH=$(pwd)/build/lib:$(pwd)/dpdk/build/lib \
  SPDK_APP_DPDK_ARGS="--file-prefix=waltest --lcores=0" \
  ./build/bin/spdk_tgt --wait-for-rpc
```
Если используется `--wait-for-rpc`, сразу после старта в другом терминале:
```sh
sudo scripts/rpc.py framework_start_init
sudo scripts/rpc.py framework_wait_init
```

## Создание устройств через rpc.py
```sh
sudo scripts/rpc.py bdev_aio_create /tmp/main.img aio_main 4096
sudo scripts/rpc.py bdev_aio_create /tmp/journal.img aio_journal 4096
sudo scripts/rpc.py wal_bdev_create -n wal0 -j aio_journal -b aio_main --block-size 4096
```
После этого `wal0` можно использовать как обычный bdev.

## Воспроизведение сбоя и восстановление
1) Остановить `spdk_tgt`, испортить основной диск, оставить журнал корректным:
```sh
sudo pkill -f spdk_tgt
printf 'WALTEST_1' | dd of=/tmp/journal.img bs=4096 count=1 seek=100 conv=notrunc
dd if=/dev/urandom of=/tmp/main.img bs=4096 count=1 seek=100 conv=notrunc
```
2) Снова запустить `spdk_tgt`, повторить `framework_start_init/framework_wait_init` (если нужно) и зарегистрировать AIO+WAL тремя командами из блока выше.
3) Запустить восстановление:
```sh
sudo scripts/rpc.py wal_bdev_recover wal0
```
4) Проверить совпадение блоков:
```sh
sudo dd if=/tmp/main.img bs=4096 count=1 skip=100 | hexdump -C | head -n 4
sudo dd if=/tmp/journal.img bs=4096 count=1 skip=100 | hexdump -C | head -n 4
```

## Альтернатива (сырой JSON)
Все шаги из `WAL_recovery_demo.md` остаются валидными при отправке RPC через `socat` на `/var/tmp/spdk.sock`.
