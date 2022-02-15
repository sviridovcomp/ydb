# Безопасные рестарт и выключение узлов

## Остановка/рестарт процесса ydb на узле {#restart_process}

Чтобы убедиться, что процесс можно остановить, надо выполнить следуюшие шаги.

1. Перейти на ноду по ssh.

1. Выполнить команду

    ```bash
    kikimr cms request restart host {node_id} --user {user} --duration 60 --dry --reason 'some-reason'
    ```

    При разрешение выведет `ALLOW`.

1. Остановить процесс

    ```bash
    sudo service kikimr stop
    ```

1. Если потребуется, запустить процесс

   ```bash
    sudo service kikimr start
    ```

## Замена оборудования {#replace_hardware}

Перед заменой нужно убедиться, что процесс ydb можно [остановить](#restart_process).  
При длительном отсутствии стоит перед этим перевезти все вдиски с данного узла и дождаться окончания репликации.  
После окончания репликации узел можно безопасно выключать.

Для отключения динамической узлы так же може потребоваться провести дрейн таблеток, дабы избежать эффекта на работающие запросы.

Стоит перейти на страницу web-мониторинга хайва или тенантного хайва.
После нажатия на кнопку "View Nodes" отобразится список всех узлов под руководством данного хайва.

В нем представлена различиная информация о запущенных таблетках и используемых ресурсов.
В правой части списка находятся кнопки со следующими действиями для каждой узла:

* **Active** - включение/отключение узла для перевоза таблеток на данный узел
* **Freeze** - запрет для таблеткок подниматься на других узлах
* **Kick** - перевоз всех таблеток разом с узла
* **Drain** - плавный перевоз всех таблеток с узла

Перед отключением узла, сначала требуется отключить перевоз таблеток через кнопку Active, после чего нажать Drain и дождаться увоза всех таблеток.