// Virtual functions, abstract classes, multiple inheritance (thunks),
// virtual destructors through delete, and pointers to members -- the
// driver interfaces of an RTOS. Each line printed is one property; the
// host's clang++ build prints the same.
#include <stdio.h>

// An abstract device: the shape of a HAL interface.
class Device {
public:
    explicit Device(const char *name) : name_(name) {}
    virtual ~Device() { printf("~Device %s\n", name_); }
    virtual int read() = 0;
    virtual void write(int v) = 0;
    virtual const char *kind() const { return "device"; }
    const char *name() const { return name_; }
private:
    const char *name_;
};

class Uart : public Device {
public:
    Uart() : Device("uart0") {}
    ~Uart() override { printf("~Uart last=%d\n", last_); }
    int read() override { return last_ * 2; }
    void write(int v) override { last_ = v; }
    const char *kind() const override { return "uart"; }
private:
    int last_ = 0;
};

class Gpio final : public Device {
public:
    explicit Gpio(int pin) : Device("gpio"), pin_(pin) {}
    ~Gpio() override { printf("~Gpio pin=%d\n", pin_); }
    int read() override { return level_; }
    void write(int v) override { level_ = v & 1; }
private:
    int pin_, level_ = 0;
};

// Multiple inheritance: an object seen through two interfaces, so calls
// through the second base go through a this-adjusting thunk.
struct Runnable {
    virtual ~Runnable() {}
    virtual int run(int x) = 0;
};
struct Named {
    virtual ~Named() {}
    virtual const char *label() const = 0;
    int tag = 7;
};
struct Task : Runnable, Named {
    int prio;
    explicit Task(int p) : prio(p) {}
    ~Task() override { printf("~Task prio=%d\n", prio); }
    int run(int x) override { return x + prio; }
    const char *label() const override { return "task"; }
};

// Calls during construction and destruction reach the class being built.
struct Base {
    Base() { printf("Base sees %s\n", who()); }
    virtual ~Base() { printf("~Base sees %s\n", who()); }
    virtual const char *who() const { return "Base"; }
};
struct Derived : Base {
    Derived() { printf("Derived sees %s\n", who()); }
    ~Derived() override { printf("~Derived sees %s\n", who()); }
    const char *who() const override { return "Derived"; }
};

// Virtual inheritance: one shared base.
struct Obj { int id = 1; virtual ~Obj() {} virtual int get() const { return id; } };
struct Left : virtual Obj { int l = 10; };
struct Right : virtual Obj { int r = 20; int get() const override { return id + r; } };
struct Both : Left, Right { int b = 30; };

struct Calc {
    int base;
    int add(int x) { return base + x; }
    int mul(int x) { return base * x; }
    virtual int vf(int x) { return base - x; }
    virtual ~Calc() {}
};
struct Calc2 : Calc {
    int vf(int x) override { return base + 100 * x; }
};
struct Pad { int z[3]; virtual void p() {} };
struct Mixed : Pad, Calc2 {};

int apply(Calc *c, int (Calc::*m)(int), int x) { return (c->*m)(x); }

int main()
{
    Device *devs[3];
    devs[0] = new Uart;
    devs[1] = new Gpio(5);
    devs[2] = new Uart;
    for (int i = 0; i < 3; i++) {
        devs[i]->write(10 + i);
        printf("%s %s read %d\n", devs[i]->name(), devs[i]->kind(),
               devs[i]->read());
    }
    for (int i = 0; i < 3; i++)
        delete devs[i];

    Task *t = new Task(3);
    Runnable *r = t;
    Named *n = t;
    printf("run %d label %s tag %d same %d\n", r->run(4), n->label(), n->tag,
           static_cast<Task *>(n) == t);
    delete n;                      // the deleting destructor, via a thunk

    {
        Derived d;
        Base &b = d;
        printf("through base %s\n", b.who());
    }

    Both both;
    Obj *o = &both;
    Left *lp = &both;
    Right *rp = &both;
    printf("vbase get %d %d %d sums %d\n", o->get(), lp->get(), rp->get(),
           both.l + both.r + both.b + lp->id);

    Calc2 c;
    c.base = 6;
    int (Calc::*m)(int) = &Calc::add;
    printf("pmf add %d\n", apply(&c, m, 4));
    m = &Calc::mul;
    printf("pmf mul %d\n", apply(&c, m, 4));
    m = &Calc::vf;
    printf("pmf virtual %d\n", apply(&c, m, 4));
    int (Calc::*nul)(int) = nullptr;
    printf("pmf null %d set %d eq %d\n", nul == nullptr, m != nullptr,
           m == &Calc::vf);
    Mixed mx;
    mx.base = 2;
    int (Mixed::*mm)(int) = &Calc::vf;   // converted: adjusted to Calc in Mixed
    printf("pmf mixed %d\n", (mx.*mm)(3));
    int Calc::*dp = &Calc::base;
    printf("pdm %d\n", c.*dp);

    printf("==END==\n");
    fflush(stdout);
    return 0;
}
