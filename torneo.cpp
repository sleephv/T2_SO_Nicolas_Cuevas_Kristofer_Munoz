#include <iostream>
#include <vector>
#include <string>
#include <pthread.h>
#include <semaphore.h>
#include <unistd.h>

using namespace std;

int main(int argc, char* argv[]) {
    if (argc < 3 || argc > 4) {
        cerr << "Uso: " << argv[0] << " <N_Jugadores> <M_Mesas> [config.txt]\n";
        return 1;
    }

    int N_jugadores = stoi(argv[1]);
    int M_mesas = stoi(argv[2]);
    
    if (N_jugadores <= 0 || M_mesas <= 0) {
        cerr << "Error: N y M deben ser mayores a 0.\n";
        return 1;
    }

    cout << "Torneo Komikan Inicializado | Jugadores: " << N_jugadores << " | Mesas: " << M_mesas << "\n";

    return 0;
}
