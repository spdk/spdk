# WAL recovery demo

Повторяемый сценарий для проверки восстановления `wal0` (journal копирует данные в main, если они отличаются).

## Сборка SPDK (однократно)
```sh
cd /home/vladimir/Documents/study/spbu/spdk
./configure
ninja -C build
```

## Hugepages перед запуском
Выделить hugepages и проверить статус (делается после перезагрузки или перед первым запуском):
```sh
cd /home/vladimir/Documents/study/spbu/spdk
sudo HUGEMEM=1024 scripts/setup.sh config   # выделить 1 ГиБ в hugepages
scripts/setup.sh status                     # убедиться, что страницы доступны
```

## Подготовка
В одном терминале (оставить запущенным):
```sh
cd /home/vladimir/Documents/study/spbu/spdk
sudo pkill -f spdk_tgt || true
sudo rm -f /var/tmp/spdk.sock /dev/shm/spdk* /var/run/.spdk* /var/run/.rte_config
dd if=/dev/zero of=/tmp/main.img bs=1M count=128
dd if=/dev/zero of=/tmp/journal.img bs=1M count=128
sudo env SPDK_APP_DPDK_ARGS="--file-prefix=waltest --lcores=0" \
  LD_LIBRARY_PATH=$(pwd)/build/lib ./build/bin/spdk_tgt
```

## Создание бэкендов и WAL
Во втором терминале:
```sh
echo '{"jsonrpc":"2.0","method":"bdev_aio_create","id":10,"params":{"name":"aio_main","filename":"/tmp/main.img","block_size":4096}}' | sudo socat - UNIX-CONNECT:/var/tmp/spdk.sock
echo '{"jsonrpc":"2.0","method":"bdev_aio_create","id":11,"params":{"name":"aio_journal","filename":"/tmp/journal.img","block_size":4096}}' | sudo socat - UNIX-CONNECT:/var/tmp/spdk.sock
echo '{"jsonrpc":"2.0","method":"wal_bdev_create","id":20,"params":{"name":"wal0","block_size":4096,"size_mb":128,"journal_name":"aio_journal","bdev_name":"aio_main"}}' | sudo socat - UNIX-CONNECT:/var/tmp/spdk.sock
```

## Имитируем крэш и расхождение данных
```sh
sudo pkill -f spdk_tgt
printf 'WALTEST_1' | dd of=/tmp/journal.img bs=4096 count=1 seek=100 conv=notrunc
dd if=/dev/urandom of=/tmp/main.img bs=4096 count=1 seek=100 conv=notrunc
```

## Перезапуск и восстановление
Запустить `spdk_tgt` снова (команда из блока «Подготовка»), затем заново зарегистрировать AIO и WAL (три RPC из блока «Создание бэкендов и WAL»), после чего:
```sh
echo '{"jsonrpc":"2.0","method":"wal_bdev_recover","id":30,"params":{"name":"wal0"}}' | sudo socat - UNIX-CONNECT:/var/tmp/spdk.sock
```

## Проверка результата
Оба дампа должны содержать `WALTEST_1` — значит main переписан из журнала:
```sh
sudo dd if=/tmp/main.img bs=4096 count=1 skip=100 | hexdump -C | head -n 4
sudo dd if=/tmp/journal.img bs=4096 count=1 skip=100 | hexdump -C | head -n 4
```
