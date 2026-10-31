# Tarea 2 - Torneo de Komikan Concurrente (Sistemas Operativos)

**Integrantes:**
- Nicolas Cuevas
- Kristofer Muñoz

---

## 1. Descripción del Proyecto
Este proyecto implementa la infraestructura completa para el primer **Torneo Multijugador de Komikan Concurrente en Tiempo Real**. A diferencia de los juegos de mesa tradicionales, este torneo **no posee turnos**. Las fichas (1 Puma y 12 Perros por mesa) operan como procesos/hilos independientes que compiten de manera simultánea por acceder a las casillas del tablero (compuesto por 28 intersecciones).

El sistema consta de:
- Un **Servidor (Matchmaker)** que empareja jugadores y asigna mesas.
- **Mesas de Juego Concurrentes** gestionadas pasivamente mediante semáforos.
- Un **Juez Árbitro** por mesa que valida movimientos, saltos de captura y declara la victoria.

## 2. Restricciones Técnicas y Compilación
- **Lenguaje:** C++ (Estándar C++17)
- **Sincronización:** Uso exclusivo de primitivas POSIX (`pthread_mutex`, `pthread_cond`, `sem_t`).

### Compilación Estricta
Se provee un `Makefile` para automatizar la compilación con los flags exigidos:
```bash
make
```
*(Comando interno ejecutado: `g++ -Wall -Wextra -std=c++17 -pthread torneo.cpp -o torneo`)*

### Modo de Uso
El programa se invoca pasando el número de jugadores ($N$), el límite de mesas ($M$) y, opcionalmente, un archivo de configuración:
```bash
./torneo <N> <M> [config.txt]
```
**Ejemplo de ejecución:**
```bash
./torneo 4 2 config.txt
```

### Limpieza de Binarios
```bash
make clean
```

---

##  3. Resumen de Diseño y Sincronización (Extracto)
- **Geometría del Tablero:** Modelado como un grafo de 28 nodos (25 de la grilla + 3 del Trono) para representar fielmente las adyacencias.
- **Prevención de Race Conditions:** Se utiliza un patrón de Monitor con un *mutex general de mesa* para arbitrar las peticiones de movimiento y *mutexes individuales por casilla* para asegurar la exclusión mutua atómica al momento de ocupar un espacio físico en el grafo.
- **Gestión de Recursos (Mesas):** Se implementó un control de acceso para garantizar que nunca se exceda el límite de $M$ mesas activas simultáneamente, manteniendo a los jugadores sobrantes en espera pasiva (sin *busy-waiting*).
- **Manejo de Señales (Resiliencia):** Captura de `SIGINT` (Ctrl+C) para abortar limpiamente las partidas activas, destruyendo los mutexes/semáforos y garantizando cero fugas de memoria.