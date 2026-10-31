#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <ctime>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>

using namespace std;

//Salto válido para que el Puma capture a un Perro

struct Salto {
    int sobre_nodo; // Casilla adyacente donde está el perro
    int destino; // Casilla donde el Puma aterriza después del salto
};

//Configuración por defecto del torneo sin necesidad de config.txt

struct Configuracion {
    int min_delay_ms = 50;
    int max_delay_ms = 300;
    unsigned int seed = 0;
    int pos_inicial_puma = 0; //El Puma empieza en el nodo 0
    vector<int> pos_inicial_perros = {15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26}; //Posiciones iniciales de los perros en el tablero
};

// Representa el estado compartido del tablero de 28 casillas de la mesa

struct TableroKomikan {
char casillas[28]; // 'P' para Puma, 'D' para Perro, ' ' para vacío
vector<int> adyacentes[28]; //Movimientos normales de 1 paso
vector<Salto> saltos_puma[28]; //Saltos de captura de 2 pasos
pthread_mutex_t mutex_casilla[28]; //Mutex individual por casilla
pthread_mutex_t mutex_tablero; //Mutex general para el tablero

TableroKomikan(const Configuracion& cfg) {
        pthread_mutex_init(&mutex_tablero, NULL);
        for (int i = 0; i < 28; i++) {
            casillas[i] = '.';
            pthread_mutex_init(&mutex_casilla[i], NULL);
        }

        construir_geometria();

        // Posicionar al Puma y a los 12 Perros según la configuración
        casillas[cfg.pos_inicial_puma] = 'P';
        for (int pos : cfg.pos_inicial_perros) {
            if (pos >= 0 && pos < 28 && pos != cfg.pos_inicial_puma) {
                casillas[pos] = 'D';
            }
        }
    }

    ~TableroKomikan() {
        pthread_mutex_destroy(&mutex_tablero);
        for (int i = 0; i < 28; i++) {
            pthread_mutex_destroy(&mutex_casilla[i]);
        }
    }

    // Agrega una arista bidireccional entre dos casillas
    void conectar(int u, int v) {
        adyacentes[u].push_back(v);
        adyacentes[v].push_back(u);
    }

    // Agrega un salto de captura en ambos sentidos de una línea de 3 nodos (a -> b -> c)
    void agregar_linea_salto(int a, int b, int c) {
        saltos_puma[a].push_back({b, c});
        saltos_puma[c].push_back({b, a});
    }

    void construir_geometria() {
        // 1. Conexiones del Trono (Nodos 0, 1, 2) acoplados a la Fila 0 (Nodos 3..7)
        conectar(0, 1); conectar(0, 5); conectar(0, 2);
        conectar(1, 2);
        conectar(1, 3); conectar(1, 4);
        conectar(2, 6); conectar(2, 7);

        // 2. Conexiones de la cuadrícula principal 5x5 (Nodos 3 al 27)
        for (int fila = 0; fila < 5; fila++) {
            for (int col = 0; col < 5; col++) {
                int actual = 3 + fila * 5 + col;
                // Derecha
                if (col < 4) conectar(actual, actual + 1);
                // Abajo
                if (fila < 4) conectar(actual, actual + 5);
            }
        }

        // 3. Saltos rectos dentro del Trono y hacia la grilla 5x5
        agregar_linea_salto(0, 1, 3); // Diagonal izquierda del trono: T -> 1 -> 3
        agregar_linea_salto(0, 2, 7); // Diagonal derecha del trono: T -> 2 -> 7
        agregar_linea_salto(0, 5, 10); // Vertical central desde T: T -> 5 -> 10
        agregar_linea_salto(1, 4, 9);  // Vertical desde nodo 1: 1 -> 4 -> 9
        agregar_linea_salto(2, 6, 11); // Vertical desde nodo 2: 2 -> 6 -> 11

        // 4. Saltos horizontales y verticales dentro de la grilla 5x5
        for (int fila = 0; fila < 5; fila++) {
            for (int col = 0; col < 5; col++) {
                int a = 3 + fila * 5 + col;
                // Salto horizontal (a -> a+1 -> a+2)
                if (col <= 2) agregar_linea_salto(a, a + 1, a + 2);
                // Salto vertical (a -> a+5 -> a+10)
                if (fila <= 2) agregar_linea_salto(a, a + 5, a + 10);
            }
        }
    }

    // Imprime el estado actual del tablero en consola

    void imprimir_tablero(int id_mesa, int perros_capturados) {
        cout << "\n========== MESA " << id_mesa << " | Perros capturados: "
             << perros_capturados << "/6 ==========\n";
        cout << "                (" << casillas[0] << ") [T:0]\n";
        cout << "              /  |  \\\n";
        cout << "        [1](" << casillas[1] << ")---+---(" << casillas[2] << ")[2]\n";
        cout << "          / |    |    | \\\n";

        for (int fila = 0; fila < 5; fila++) {
            cout << "        ";
            for (int col = 0; col < 5; col++) {
                int idx = 3 + fila * 5 + col;
                cout << "(" << casillas[idx] << ")";
                if (col < 4) cout << "---";
            }
            cout << "\n";
            if (fila < 4) {
                cout << "         |     |     |     |     |\n";
            }
        }
        cout << "====================================================\n";
    }
};

// Función para leer config.txt y cargar la configuración del torneo

Configuracion cargar_configuracion(const string& ruta) {
    Configuracion cfg;
    cfg.seed = static_cast<unsigned int>(time(NULL));

    if (ruta.empty()) return cfg;

    ifstream archivo(ruta);
    if (!archivo.is_open()) {
        cerr << "Aviso: No se pudo abrir " << ruta << ". Usando valores por defecto.\n";
        return cfg;
    }

    string linea;
    while (getline(archivo, linea)) {
        if (linea.empty() || linea[0] == '#') continue;
        stringstream ss(linea);
        string clave;
        if (getline(ss, clave, '=')) {
            string valor;
            getline(ss, valor);
            if (clave == "min_delay") cfg.min_delay_ms = stoi(valor);
            else if (clave == "max_delay") cfg.max_delay_ms = stoi(valor);
            else if (clave == "seed") cfg.seed = static_cast<unsigned int>(stoul(valor));
            else if (clave == "puma") cfg.pos_inicial_puma = stoi(valor);
            else if (clave == "perros") {
                cfg.pos_inicial_perros.clear();
                stringstream ss_perros(valor);
                string pos_str;
                while (getline(ss_perros, pos_str, ',')) {
                    cfg.pos_inicial_perros.push_back(stoi(pos_str));
                }
            }
        }
    }
    return cfg;
};


int main(int argc, char* argv[]) {
    if (argc < 3 || argc > 4) {
        cerr << "Uso: " << argv[0] << " <N_Jugadores> <M_Mesas> [config.txt]\n";
        return 1;
    }

    int N = stoi(argv[1]);
    int M = stoi(argv[2]);
    string ruta_cfg = (argc == 4) ? argv[3] : "";

    if (N <= 0 || M <= 0) {
        cerr << "Error: N y M deben ser mayores a 0.\n";
        return 1;
    }

    Configuracion cfg = cargar_configuracion(ruta_cfg);
    srand(cfg.seed);

    cout << "Torneo Komikan Inicializado | N=" << N << " jugadores | M=" << M << " mesas max\n";
    cout << "Delays configurados: [" << cfg.min_delay_ms << " ms - " << cfg.max_delay_ms << " ms]\n";

    // Prueba visual de la Etapa 1: crear un tablero y mostrarlo
    TableroKomikan tablero_prueba(cfg);
    tablero_prueba.imprimir_tablero(1, 0);

    return 0;
};