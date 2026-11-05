// C++23's operator[] of any number of arguments: a matrix's m[i, j], a
// three-dimensional view, none (m[]), a braced argument, a pack expanded
// into the brackets, a static operator[]; a one-argument operator[] still
// takes a single index, and an array's [] is the built-in one.
// expect-exit: 42
// std: c++23
struct Matrix {
    int cells[3][4];
    int &operator[](int i, int j) { return cells[i][j]; }
    const int &operator[](int i, int j) const { return cells[i][j]; }
    int operator[]() const { return 3 * 4; }
};
struct Cube {
    int d[2][2][2];
    int &operator[](int i, int j, int k) { return d[i][j][k]; }
};
struct Pt { int x, y; };
struct Grid {
    int at(Pt p) const { return p.x * 10 + p.y; }
    int operator[](Pt p) const { return at(p); }
};
struct Static {
    static int operator[](int a, int b) { return a - b; }
};
struct Vec {
    int v[4] = { 5, 6, 7, 8 };
    int &operator[](int i) { return v[i]; }
};
template <class M, class... I> int &pick(M &m, I... i) { return m[i...]; }

int main()
{
    int fails = 0;
    Matrix m{};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++)
            m[i, j] = i * 4 + j;
    fails += m[2, 3] != 11;
    const Matrix &cm = m;
    fails += cm[1, 2] != 6;
    fails += m[] != 12;
    Cube c{};
    c[1, 0, 1] = 9;
    fails += c.d[1][0][1] != 9;
    fails += pick(c, 1, 0, 1) != 9;
    pick(m, 0, 0) = 40;
    fails += m.cells[0][0] != 40;
    Grid g;
    fails += g[{3, 4}] != 34;
    fails += Static{}[9, 2] != 7;
    Vec v;
    fails += v[2] != 7;
    int a[3] = { 1, 2, 3 };
    fails += a[1] != 2;
    return fails == 0 ? 42 : 1;
}
