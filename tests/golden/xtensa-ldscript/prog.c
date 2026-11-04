/* Under the script, a table and a string in .rodata, which the script puts in a RAM region of
 * their own (as the ESP32's instruction RAM takes only 32-bit loads, so byte data must not stay
 * with the code), initialized and zeroed data, and a constructor. main returns 42 when every
 * byte read back is right. */
static const unsigned char table[] = {3, 1, 4, 1, 5, 9, 2, 6};
static const char msg[] = "rodata in its own region";
int counter = 7;
static int zeroed[4];
static int constructed;
__attribute__((constructor)) static void ctor(void) { constructed = 5; }
const void *where_msg(void) { return msg; }

int main(void)
{
    int sum = 0;
    for (unsigned i = 0; i < sizeof table; i++)
        sum += table[i];                 /* byte loads from .rodata */
    for (unsigned i = 0; msg[i]; i++)
        sum += msg[i] == ' ';            /* 4 spaces */
    for (unsigned i = 0; i < 4; i++)
        sum += zeroed[i];
    /* 31 + 4 + 0 + 7 = 42, when the constructor ran */
    return constructed == 5 ? sum + counter : 1;
}
