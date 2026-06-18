# Contrato para el motor de almacenamiento

El archivo público `include/storage.h` es la frontera entre ambas mitades del
proyecto. `src/server.c` solo llama esas funciones y no conoce el formato
interno de los buckets.

El backend actual, `src/storage_mock.c`, guarda objetos en archivos temporales
sin nombre y mantiene sus índices en memoria. Existe exclusivamente para
probar el cliente y se pierde al reiniciar el servidor.

El backend persistente deberá:

1. Reemplazar `src/storage_mock.c` en la lista `SERVER_SRC` del `Makefile`.
2. Implementar todas las funciones declaradas en `storage.h`.
3. Consumir exactamente `size` bytes de `input_fd` en `storage_put_object`.
4. Entregar en `storage_reader_t` un descriptor legible, desplazamiento,
   tamaño y fecha. El servidor usa `pread`, por lo que no depende de la
   posición actual del descriptor.
5. Devolver uno de los estados definidos en `protocol.h` y escribir un mensaje
   breve en el buffer `error`.
6. Mantener vigente el descriptor del lector hasta `storage_close_reader`.

La implementación persistente es responsable del bloque de directorio de
1 MiB, tabla de objetos, lista de espacios libres, primer ajuste, división y
coalescencia. Esos detalles no deben filtrarse al cliente ni al servidor.

