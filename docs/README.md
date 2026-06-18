# Guía técnica del proyecto

Esta carpeta contiene la explicación técnica necesaria para entender el
proyecto y preparar las pruebas. No hace falta leer todo el código antes de
comenzar.

## Orden recomendado de lectura

1. Leer el `README.md` principal para conocer el problema, los comandos y la
   forma de ejecutar los programas.
2. Leer [`protocol.md`](protocol.md) para entender cómo se comunican el cliente
   y el servidor.
3. Leer [`storage-interface.md`](storage-interface.md) para entender cómo se
   guardan los buckets y objetos.

## Flujo general

```text
Usuario
  |
  v
aws-s3 (cliente)
  |
  | Solicitud TCP
  v
aws-s3_server
  |
  | Llamada a storage_*
  v
Motor de almacenamiento
  |
  v
archivo data/<bucket>.s3b
```

El cliente interpreta los comandos y se encarga de recorrer los directorios
locales. El servidor recibe solicitudes, las valida y llama al motor de
almacenamiento. El motor administra los archivos `.s3b`.

## Responsabilidad de cada archivo

- `C/client.c`: comandos, opciones, recorridos locales y sincronización.
- `C/server.c`: socket de escucha, recepción y despacho de solicitudes.
- `C/protocol.c`: serialización de los mensajes enviados por TCP.
- `C/storage_file.c`: buckets, objetos y espacios libres.
- `C/uri.c`: validación de direcciones `s3://`.
- `C/manifest.c`: listas de archivos utilizadas por operaciones recursivas.
- `C/common.c`: lectura, escritura, conexiones y funciones compartidas.

## Qué debe comprobarse en las pruebas

### Comandos

- `ls` sin argumentos lista buckets.
- `ls s3://bucket/prefijo/` muestra objetos y prefijos.
- `mb` crea un bucket y rechaza nombres repetidos.
- `cp` funciona de local a S3, de S3 a local y entre ubicaciones S3.
- `mv` copia primero y elimina el origen solamente si la copia funciona.
- `rm --recursive` elimina todos los objetos de un prefijo.
- `sync` funciona en ambas direcciones.
- `sync --delete` elimina elementos sobrantes en el destino.
- `rb` rechaza buckets no vacíos y `rb --force` sí los elimina.

### Tipos de archivo

- Archivos de texto.
- Archivos binarios.
- Archivos vacíos.
- Archivos mayores a 64 KiB.
- Nombres con espacios.
- Estructuras con varios subdirectorios.

### Persistencia

Después de subir archivos:

1. Detener el servidor.
2. Iniciarlo nuevamente con el mismo directorio `--data`.
3. Listar y descargar los objetos.
4. Compararlos con los originales usando `cmp` o `sha256sum`.

### Administración del espacio

- Reemplazar un objeto por otro del mismo tamaño no debe moverlo.
- Reemplazarlo por uno de tamaño diferente debe escribir la nueva versión al
  final y liberar el espacio anterior.
- Un objeto nuevo debe reutilizar el primer espacio libre suficientemente
  grande.
- Al eliminar espacios contiguos, estos deben unirse.

### Rendimiento

Se recomienda medir:

- Tiempo de subida y descarga.
- Tamaño del archivo `.s3b`.
- Consumo aproximado de memoria.
- Diferencias entre muchos archivos pequeños y un archivo grande.

Los resultados, comandos utilizados y salidas importantes deben copiarse al
documento final del proyecto.
