# Proyecto #3 de Principios de Sistemas Operativos

Cliente y servidor en C11 que simulan almacenamiento de objetos estilo AWS S3
sobre TCP. Esta rama implementa la capa de cliente, protocolo, red y el contrato
del motor de almacenamiento.

## Compilación

Requisitos: Linux o WSL, GCC y GNU Make.

```sh
make
```

Se generan:

- `bin/aws-s3`
- `bin/aws-s3_server`

## Ejecución

Inicie el servidor de prueba:

```sh
bin/aws-s3_server --host 127.0.0.1 --port 9000
```

En otra terminal:

```sh
bin/aws-s3 mb s3://ejemplo
bin/aws-s3 cp archivo.bin s3://ejemplo/
bin/aws-s3 ls s3://ejemplo/
bin/aws-s3 cp s3://ejemplo/archivo.bin ./descarga.bin
bin/aws-s3 rm s3://ejemplo/archivo.bin
bin/aws-s3 rb s3://ejemplo
```

El cliente acepta `--host` y `--port`, además de las variables
`AWS_S3_HOST` y `AWS_S3_PORT`.

Comandos disponibles:

- `ls [s3://bucket/prefijo] [--recursive]`
- `mb s3://bucket`
- `cp ORIGEN DESTINO [--recursive]`
- `mv ORIGEN DESTINO [--recursive]`
- `rm s3://bucket/clave [--recursive]`
- `sync ORIGEN DESTINO [--delete]`
- `rb s3://bucket [--force]`

No se siguen enlaces simbólicos. Los archivos se transmiten por streaming en
bloques de 64 KiB.

## Pruebas

```sh
make test
```

Incluye pruebas de URIs, serialización, recepción fragmentada y una integración
cliente-servidor con comparación de un archivo binario.

## Estado de la división

Esta implementación usa un backend falso y no persistente para poder probar
todos los comandos. La segunda mitad del equipo debe sustituirlo por el motor
de archivos `.s3b` requerido por el enunciado. La interfaz congelada y las
obligaciones del reemplazo se describen en
[`docs/storage-interface.md`](docs/storage-interface.md). El protocolo está
documentado en [`docs/protocol.md`](docs/protocol.md).
