/* vector<string>: the two containers together, which is where a
 * self-referential element type and a relocating container meet. */
#include <vector>
#include <string>
#include "check.h"
using namespace std;
int main()
{
    vector<string> v;
    for (int i = 0; i < 40; i++)
        v.push_back(string("item-") + to_string(i));
    CHECK(v.size() == 40);
    CHECK(v[0] == "item-0");
    CHECK(v[39] == "item-39");
    /* The vector reallocated several times; every short string must
     * have been re-aimed at its new home each time. */
    for (size_t i = 0; i < v.size(); i++)
        CHECK(v[i] == string("item-") + to_string((int)i));
    /* A long element too, so both string layouts travel. */
    v.push_back(string(200, 'z'));
    CHECK(v.back().size() == 200 && v.back()[199] == 'z');
    for (int i = 0; i < 40; i++) v.push_back("more");
    CHECK(v[40].size() == 200);
    vector<string> w = v;
    CHECK(w == v);
    w[0] = "changed";
    CHECK(v[0] == "item-0");
    DONE();
}
