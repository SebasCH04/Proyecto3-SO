# Carpeta de pruebas

Esta carpeta contiene scripts reproducibles para validar el comportamiento del
proyecto y recopilar datos para el informe.

## Archivos

- `funcionales.sh`: comprueba operaciones básicas del cliente y el servidor.
- `validacion.sh`: verifica persistencia, reemplazo de objetos y manejo de
  errores esperados.
- `rendimiento.sh`: mide tiempos de subida y descarga para un archivo grande y
  para un conjunto de archivos pequeños.

## Uso

Primero compile el proyecto:

```sh
make
```

Luego ejecute el script deseado:

```sh
sh pruebas/funcionales.sh
sh pruebas/validacion.sh
sh pruebas/rendimiento.sh
```

Cada script crea un entorno temporal propio bajo `/tmp`, inicia un servidor de
prueba, ejecuta sus operaciones y elimina los archivos al finalizar.
