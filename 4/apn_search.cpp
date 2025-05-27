/*  -----------------------------------------------------------
    apn_search.cpp
    brute‑force CCZ‑поворотов вида  (x',y') = (x ⊕ B·F(x), F(x))
    для 8‑битной квадратичной APN‑функции Example‑1 (δ = 2,  но не биекция).
    Цель –  найти CCZ‑эквивалентную APN‑пермутацию (δ = 2 и биекция).
    Автор: ChatGPT (o3), 2025‑04‑16
    ----------------------------------------------------------- */

    #include <array>
    #include <bitset>
    #include <chrono>
    #include <cstdint>
    #include <iostream>
    #include <random>
    
    using std::array;
    using std::uint8_t;
    using std::uint32_t;
    using Clock = std::chrono::steady_clock;
    
    /* ---------- 8‑битная APN‑функция (Example 1) ---------------- */
    static const uint8_t F[256] = {
    0,0,0,236,0,20,164,92,0,25,100,145,179,190,115,146,0,231,122,113,105,154,183,168,
    119,137,105,123,173,71,23,17,0,239,131,128,29,230,58,45,213,35,50,40,123,153,56,
    54,148,156,109,137,224,252,189,77,54,39,171,86,241,244,200,33,0,73,32,133,72,21,
    204,125,197,149,129,61,62,122,222,118,14,160,84,22,47,149,209,135,188,11,130,217,
    46,141,180,251,62,152,157,215,107,217,108,50,46,145,233,186,200,99,171,236,164,
    229,125,208,152,205,229,92,195,155,126,202,76,0,85,245,0,87,77,246,49,114,216,119,
    139,197,162,0,9,83,132,50,11,187,60,96,83,247,192,136,247,94,164,225,28,161,235,
    186,195,123,13,89,239,67,133,197,157,60,55,122,2,183,12,85,92,3,232,91,25,82,9,
    174,117,51,165,15,131,209,247,73,123,101,22,228,2,8,203,45,53,50,60,215,255,236,
    82,173,126,135,105,124,110,131,221,220,71,167,52,56,228,16,51,43,134,119,104,117,
    226,7,168,161,29,245,151,147,202,54,228,244,23,1,131,121,26,24,42,196,251,244,11,
    232,69,94,17,230};
    
    /* ---------- вспомогательные утилиты ------------------------ */
    inline uint8_t parity8(uint32_t x) { return __builtin_parity(x); }
    
    bool is_permutation(const uint8_t lut[256])
    {
        std::bitset<256> seen;
        for (uint32_t x = 0; x < 256; ++x) {
            if (seen.test(lut[x])) return false;
            seen.set(lut[x]);
        }
        return true;
    }
    
    /* дифференциальная однородность, early‑exit >2 */
    uint8_t differential_uniformity(const uint8_t lut[256])
    {
        uint8_t maxCnt = 0;
        uint8_t diffCnt[256];
    
        for (uint32_t a = 1; a < 256; ++a) {
            std::fill(std::begin(diffCnt), std::end(diffCnt), 0);
            for (uint32_t x = 0; x < 256; ++x) {
                uint8_t b = lut[x] ^ lut[x ^ a];
                if (++diffCnt[b] > 2) return 4;      // δ > 2, можно выйти
            }
            for (uint32_t b = 0; b < 256; ++b)
                if (diffCnt[b] > maxCnt) maxCnt = diffCnt[b];
        }
        return maxCnt;                               // 2 или 4
    }
    
    /* ---------- одна попытка с рандомной матрицей B ------------- */
    bool try_random_B(std::mt19937_64 &rng,
                      std::array<uint8_t,256> &G_out)
    {
        /* 8×8 бинарная матрица B, задаём 8 строк‑масок */
        std::array<uint8_t,8> B;
        for (auto &row : B) row = static_cast<uint8_t>(rng() & 0xFF);
    
        /* предвычисляем T[y] = B·y  */
        uint8_t T[256];
        for (uint32_t y = 0; y < 256; ++y) {
            uint8_t v = 0;
            for (uint8_t bit = 0; bit < 8; ++bit)
                v |= parity8(B[bit] & y) << bit;
            T[y] = v;
        }
    
        /* строим x' = x ⊕ T[F[x]] и LUT G[x'] = F[x] */
        std::bitset<256> seenX, seenY;
        std::array<uint8_t,256> G;
    
        for (uint32_t x = 0; x < 256; ++x) {
            uint8_t xp = static_cast<uint8_t>(x) ^ T[F[x]];
            if (seenX.test(xp)) return false;         // не функция
            seenX.set(xp);
            if (seenY.test(F[x])) return false;       // сразу проверим биекцию по y
            seenY.set(F[x]);
            G[xp] = F[x];
        }
        if (!seenY.all()) return false;               // не биекция
    
        /* δ */
        if (differential_uniformity(G.data()) != 2) return false;
    
        /* успех, копируем LUT наружу */
        G_out = G;
        return true;
    }
    
    /* ---------------------- main -------------------------------- */
    int main()
    {
        std::mt19937_64 rng(std::random_device{}());
        const uint64_t reportEach = 1'000'000;        // прогресс каждые N матриц
        uint64_t iter = 0;
    
        auto t0 = Clock::now();
        std::array<uint8_t,256> G;
    
        std::cout << "Searching for 8‑bit APN permutation (δ=2)…\n";
        while (true) {
            ++iter;
            if (try_random_B(rng, G)) {
                auto t1 = Clock::now();
                double sec = std::chrono::duration<double>(t1 - t0).count();
                std::cout << "\n=== JACKPOT after " << iter
                          << " matrices, time " << sec << " s ===\n";
                std::cout << "LUT (hex):\n";
                for (int i = 0; i < 256; ++i) {
                    std::cout << std::hex << std::uppercase
                              << (G[i] >> 4) << (G[i] & 0xF) << (i % 16 == 15 ? "\n" : " ");
                }
                return 0;
            }
    
            if (iter % reportEach == 0) {
                auto t1 = Clock::now();
                double sec = std::chrono::duration<double>(t1 - t0).count();
                std::cout << "Iter " << iter << "…  elapsed " << sec << " s\r" << std::flush;
            }
        }
    }
    