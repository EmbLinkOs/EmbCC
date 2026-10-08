/* 64-bit constants from a literal pool (Cortex-M: `ldrd rlo, rhi, [pc,
 * #n]` and the value after the function -- t_lit64 in
 * src/arch/thumb/codegen.c), checked by value:
 *
 *   - mix() names 160 different constants, so its code runs far past the
 *     1020 bytes an ldrd reaches: the first ones must fall back to
 *     movw/movt, the rest load from the pool, and both give the value;
 *   - the same constant used several times shares one pool slot;
 *   - doubles from the pool, through the soft-float helpers.
 * The expected value is computed with the constants XOR-ed in pairs at
 * run time from halves the compiler cannot fold. */
// expect-exit: 42
typedef unsigned long long u64;
static volatile unsigned salt;

__attribute__((noinline)) u64 step(u64 x, u64 v, int i)
{
    switch (i % 3) {
    case 0: x = x ^ v; break;
    case 1: x = x + v; break;
    default: x = x - v; break;
    }
    return x * 3 + salt;
}

__attribute__((noinline)) u64 mix(u64 x)
{
    x = step(x, 0xf2a74de452e6b438ULL, 0);
    x = step(x, 0x6513270e269e0d37ULL, 1);
    x = step(x, 0x0c5c7fd0a6a3a450ULL, 2);
    x = step(x, 0xd23f0824128b2f33ULL, 3);
    x = step(x, 0x1818e811892f902bULL, 4);
    x = step(x, 0x9531985d5d9dc9f8ULL, 5);
    x = step(x, 0xe8e25d940ed90475ULL, 6);
    x = step(x, 0x36f675cc81e74ef5ULL, 7);
    x = step(x, 0x1600a35a099950d8ULL, 8);
    x = step(x, 0x6b0d549b6f03675aULL, 9);
    x = step(x, 0x3d9c172411e20b8fULL, 10);
    x = step(x, 0x8d116ece1738f7d9ULL, 11);
    x = step(x, 0x0f21ddb66cad4a26ULL, 12);
    x = step(x, 0x90c192cfd3ac94afULL, 13);
    x = step(x, 0xf28c105d1fb17c23ULL, 14);
    x = step(x, 0xa170b33839263059ULL, 15);
    x = step(x, 0x953f48f1a09f76b5ULL, 16);
    x = step(x, 0x0fd630f1f29d0da9ULL, 17);
    x = step(x, 0x95e60af593bd04cfULL, 18);
    x = step(x, 0x0cb1e29c658cda14ULL, 19);
    x = step(x, 0x3898d190f9ebdaccULL, 20);
    x = step(x, 0x8e81973e0becd7b0ULL, 21);
    x = step(x, 0x2217beaddbc496cbULL, 22);
    x = step(x, 0x6b4cb2424a23d596ULL, 23);
    x = step(x, 0x8a6a63ec24ede6a4ULL, 24);
    x = step(x, 0x922766581e27a1c0ULL, 25);
    x = step(x, 0x8f6d05584ef8aa38ULL, 26);
    x = step(x, 0xae97ba94d0eda82fULL, 27);
    x = step(x, 0x1a61dbe22e44158bULL, 28);
    x = step(x, 0x923a736994e3bf91ULL, 29);
    x = step(x, 0x301850c5a38fd547ULL, 30);
    x = step(x, 0x18f135d25f557203ULL, 31);
    x = step(x, 0xb64ce4228c38fb29ULL, 32);
    x = step(x, 0x907a70c31012f037ULL, 33);
    x = step(x, 0x9e7769b10f4205b4ULL, 34);
    x = step(x, 0x7f15052434b9b5dfULL, 35);
    x = step(x, 0x881ed162ae2eb154ULL, 36);
    x = step(x, 0xc6f877186d76b07eULL, 37);
    x = step(x, 0x7731af10506bf2efULL, 38);
    x = step(x, 0xec66a78795e761d1ULL, 39);
    x = step(x, 0x5c90a9587403e430ULL, 40);
    x = step(x, 0x3f98e2774cbd87adULL, 41);
    x = step(x, 0x2e05319acb5c7427ULL, 42);
    x = step(x, 0xc7a2ea20b2f14c94ULL, 43);
    x = step(x, 0x14f4733f3e7d1bfbULL, 44);
    x = step(x, 0x4cdd2055930d6eafULL, 45);
    x = step(x, 0x7ebff20686734721ULL, 46);
    x = step(x, 0x57ee05cde00902c7ULL, 47);
    x = step(x, 0x72e6cc3ababced20ULL, 48);
    x = step(x, 0x9be4bcfc49b64a08ULL, 49);
    x = step(x, 0x12bd4acefaecbd38ULL, 50);
    x = step(x, 0x830e07bc1e398f10ULL, 51);
    x = step(x, 0x2a3af4d46b0a18e8ULL, 52);
    x = step(x, 0x5790f82ec1d3fcffULL, 53);
    x = step(x, 0xeeeacbe226e87555ULL, 54);
    x = step(x, 0x6bf46c697d2caf82ULL, 55);
    x = step(x, 0xf646e1f40a097c97ULL, 56);
    x = step(x, 0x13deef86ab1031d0ULL, 57);
    x = step(x, 0x8ede0d7ac3baea9eULL, 58);
    x = step(x, 0xca02135e92b1d3f2ULL, 59);
    x = step(x, 0xd17f9acae01f5057ULL, 60);
    x = step(x, 0x571242425051c1ccULL, 61);
    x = step(x, 0x59a54a7bb1fee08fULL, 62);
    x = step(x, 0x7f26144b98289fcdULL, 63);
    x = step(x, 0xcc011cdd9474031bULL, 64);
    x = step(x, 0x119a72d174c9df6aULL, 65);
    x = step(x, 0x17f5e837d70820feULL, 66);
    x = step(x, 0x451abd81f1d69ed6ULL, 67);
    x = step(x, 0xb2715945795e8229ULL, 68);
    x = step(x, 0x10a3d6b2aa05e11aULL, 69);
    x = step(x, 0xbb2d420f0f88080bULL, 70);
    x = step(x, 0x4f426dcbb394fb36ULL, 71);
    x = step(x, 0x93f448b3a5aa3c81ULL, 72);
    x = step(x, 0xae658f33fe3b890bULL, 73);
    x = step(x, 0x72158370d269a9a5ULL, 74);
    x = step(x, 0xb774eb5248db40afULL, 75);
    x = step(x, 0xe315128862c33a4fULL, 76);
    x = step(x, 0x58d5563dab2cd31eULL, 77);
    x = step(x, 0xf0ce583505c6af07ULL, 78);
    x = step(x, 0x5affb2297631a992ULL, 79);
    x = step(x, 0x9c6539382b0537e6ULL, 80);
    x = step(x, 0x7e62aa0a1df9fd78ULL, 81);
    x = step(x, 0x37dc76fb0f17a300ULL, 82);
    x = step(x, 0x49952399c4aaeac1ULL, 83);
    x = step(x, 0xbd0561e6211c70cfULL, 84);
    x = step(x, 0x65dc9f503f63af83ULL, 85);
    x = step(x, 0xeab477d26415479cULL, 86);
    x = step(x, 0x7f1b103cdf1582b0ULL, 87);
    x = step(x, 0x2a96fb1a14a0f9e7ULL, 88);
    x = step(x, 0x66d2287672fdf202ULL, 89);
    x = step(x, 0x4720771f8ca81811ULL, 90);
    x = step(x, 0x230d977ee2257159ULL, 91);
    x = step(x, 0x6e36aab0d1bc52d9ULL, 92);
    x = step(x, 0x8cdb305fdd2e1609ULL, 93);
    x = step(x, 0xb4d66a3a47469a4dULL, 94);
    x = step(x, 0xfc891b4a6a50df4dULL, 95);
    x = step(x, 0xaec6f0245bd86d40ULL, 96);
    x = step(x, 0x616499c9e25a7605ULL, 97);
    x = step(x, 0x3b1287fff52ddf5dULL, 98);
    x = step(x, 0x153e7c2a26a2c0bdULL, 99);
    x = step(x, 0x26bb7dbd2d1c9af0ULL, 100);
    x = step(x, 0xa8948c893b618676ULL, 101);
    x = step(x, 0x0316909e3bbbe9eaULL, 102);
    x = step(x, 0xd4c28c2e7c26847fULL, 103);
    x = step(x, 0x2eae05cf96d0cc5fULL, 104);
    x = step(x, 0x482c9cbc43435cc5ULL, 105);
    x = step(x, 0x254b0c4e010c4759ULL, 106);
    x = step(x, 0x88daf4016b4013efULL, 107);
    x = step(x, 0x9c1caaf75e8766edULL, 108);
    x = step(x, 0x519088f590fbbd11ULL, 109);
    x = step(x, 0x20203626f3fe39c0ULL, 110);
    x = step(x, 0xdbf4a8b2b0c4312dULL, 111);
    x = step(x, 0xf341e07a83f73f16ULL, 112);
    x = step(x, 0xa7abe1c29e1a8ef4ULL, 113);
    x = step(x, 0xbd628881ad1b72dbULL, 114);
    x = step(x, 0x74e69a5d0dd27a65ULL, 115);
    x = step(x, 0xdef88334e647cb8fULL, 116);
    x = step(x, 0xf3aed0b6c7ac1491ULL, 117);
    x = step(x, 0xae3a2b7fdfe01893ULL, 118);
    x = step(x, 0x8f2c6ec8cc4169a3ULL, 119);
    x = step(x, 0x65e7e4236472f1a3ULL, 120);
    x = step(x, 0x64e50cad66237a04ULL, 121);
    x = step(x, 0x7b45145c1a81682cULL, 122);
    x = step(x, 0x66836886a260cd0bULL, 123);
    x = step(x, 0x30cbc97d0fef7928ULL, 124);
    x = step(x, 0xfc132d0d113db17dULL, 125);
    x = step(x, 0x70ccec313571810aULL, 126);
    x = step(x, 0x1c2442f9298cb3a5ULL, 127);
    x = step(x, 0x99c94309570dc195ULL, 128);
    x = step(x, 0x1a358ca00d75985dULL, 129);
    x = step(x, 0x9118bb16000f49c8ULL, 130);
    x = step(x, 0x895fd7b326b94c7fULL, 131);
    x = step(x, 0xf2ee4e4519f9919cULL, 132);
    x = step(x, 0x9d1de2a05d158a2fULL, 133);
    x = step(x, 0x1200339d068739faULL, 134);
    x = step(x, 0x353c631cdfd43f37ULL, 135);
    x = step(x, 0x6050914a9d33a01cULL, 136);
    x = step(x, 0xa268aa872607679dULL, 137);
    x = step(x, 0xf4998d7c4093f6deULL, 138);
    x = step(x, 0x9a2ef80f58ee8571ULL, 139);
    x = step(x, 0x7961fd925d39d0a8ULL, 140);
    x = step(x, 0x1d87cec31f7296abULL, 141);
    x = step(x, 0x7cf20724d953ee26ULL, 142);
    x = step(x, 0xfa529ba3fe3bfadaULL, 143);
    x = step(x, 0x7afb2c68774b15d7ULL, 144);
    x = step(x, 0x4fd58dbe7bdc968bULL, 145);
    x = step(x, 0x24e4e25a15fc899eULL, 146);
    x = step(x, 0xbfeaa1551a28f7b3ULL, 147);
    x = step(x, 0xbd87a86557b6fb7eULL, 148);
    x = step(x, 0x7a86f7a243c71b9aULL, 149);
    x = step(x, 0xb12aa1f6d42fddbbULL, 150);
    x = step(x, 0x842e7fc229540a6eULL, 151);
    x = step(x, 0x3488f87605e999f3ULL, 152);
    x = step(x, 0xf3b7a50df373ca53ULL, 153);
    x = step(x, 0x5c9bcf35873be078ULL, 154);
    x = step(x, 0xb0a844e52587be6bULL, 155);
    x = step(x, 0xea0575438b0d590bULL, 156);
    x = step(x, 0xc215a82a06ec41adULL, 157);
    x = step(x, 0x4c4f9b0687322e25ULL, 158);
    x = step(x, 0xa49636a2fa7f0eabULL, 159);
    return x;
}

static const unsigned halves[160][2] = {
    { 0xf2a74de4u, 0x52e6b438u },
    { 0x6513270eu, 0x269e0d37u },
    { 0x0c5c7fd0u, 0xa6a3a450u },
    { 0xd23f0824u, 0x128b2f33u },
    { 0x1818e811u, 0x892f902bu },
    { 0x9531985du, 0x5d9dc9f8u },
    { 0xe8e25d94u, 0x0ed90475u },
    { 0x36f675ccu, 0x81e74ef5u },
    { 0x1600a35au, 0x099950d8u },
    { 0x6b0d549bu, 0x6f03675au },
    { 0x3d9c1724u, 0x11e20b8fu },
    { 0x8d116eceu, 0x1738f7d9u },
    { 0x0f21ddb6u, 0x6cad4a26u },
    { 0x90c192cfu, 0xd3ac94afu },
    { 0xf28c105du, 0x1fb17c23u },
    { 0xa170b338u, 0x39263059u },
    { 0x953f48f1u, 0xa09f76b5u },
    { 0x0fd630f1u, 0xf29d0da9u },
    { 0x95e60af5u, 0x93bd04cfu },
    { 0x0cb1e29cu, 0x658cda14u },
    { 0x3898d190u, 0xf9ebdaccu },
    { 0x8e81973eu, 0x0becd7b0u },
    { 0x2217beadu, 0xdbc496cbu },
    { 0x6b4cb242u, 0x4a23d596u },
    { 0x8a6a63ecu, 0x24ede6a4u },
    { 0x92276658u, 0x1e27a1c0u },
    { 0x8f6d0558u, 0x4ef8aa38u },
    { 0xae97ba94u, 0xd0eda82fu },
    { 0x1a61dbe2u, 0x2e44158bu },
    { 0x923a7369u, 0x94e3bf91u },
    { 0x301850c5u, 0xa38fd547u },
    { 0x18f135d2u, 0x5f557203u },
    { 0xb64ce422u, 0x8c38fb29u },
    { 0x907a70c3u, 0x1012f037u },
    { 0x9e7769b1u, 0x0f4205b4u },
    { 0x7f150524u, 0x34b9b5dfu },
    { 0x881ed162u, 0xae2eb154u },
    { 0xc6f87718u, 0x6d76b07eu },
    { 0x7731af10u, 0x506bf2efu },
    { 0xec66a787u, 0x95e761d1u },
    { 0x5c90a958u, 0x7403e430u },
    { 0x3f98e277u, 0x4cbd87adu },
    { 0x2e05319au, 0xcb5c7427u },
    { 0xc7a2ea20u, 0xb2f14c94u },
    { 0x14f4733fu, 0x3e7d1bfbu },
    { 0x4cdd2055u, 0x930d6eafu },
    { 0x7ebff206u, 0x86734721u },
    { 0x57ee05cdu, 0xe00902c7u },
    { 0x72e6cc3au, 0xbabced20u },
    { 0x9be4bcfcu, 0x49b64a08u },
    { 0x12bd4aceu, 0xfaecbd38u },
    { 0x830e07bcu, 0x1e398f10u },
    { 0x2a3af4d4u, 0x6b0a18e8u },
    { 0x5790f82eu, 0xc1d3fcffu },
    { 0xeeeacbe2u, 0x26e87555u },
    { 0x6bf46c69u, 0x7d2caf82u },
    { 0xf646e1f4u, 0x0a097c97u },
    { 0x13deef86u, 0xab1031d0u },
    { 0x8ede0d7au, 0xc3baea9eu },
    { 0xca02135eu, 0x92b1d3f2u },
    { 0xd17f9acau, 0xe01f5057u },
    { 0x57124242u, 0x5051c1ccu },
    { 0x59a54a7bu, 0xb1fee08fu },
    { 0x7f26144bu, 0x98289fcdu },
    { 0xcc011cddu, 0x9474031bu },
    { 0x119a72d1u, 0x74c9df6au },
    { 0x17f5e837u, 0xd70820feu },
    { 0x451abd81u, 0xf1d69ed6u },
    { 0xb2715945u, 0x795e8229u },
    { 0x10a3d6b2u, 0xaa05e11au },
    { 0xbb2d420fu, 0x0f88080bu },
    { 0x4f426dcbu, 0xb394fb36u },
    { 0x93f448b3u, 0xa5aa3c81u },
    { 0xae658f33u, 0xfe3b890bu },
    { 0x72158370u, 0xd269a9a5u },
    { 0xb774eb52u, 0x48db40afu },
    { 0xe3151288u, 0x62c33a4fu },
    { 0x58d5563du, 0xab2cd31eu },
    { 0xf0ce5835u, 0x05c6af07u },
    { 0x5affb229u, 0x7631a992u },
    { 0x9c653938u, 0x2b0537e6u },
    { 0x7e62aa0au, 0x1df9fd78u },
    { 0x37dc76fbu, 0x0f17a300u },
    { 0x49952399u, 0xc4aaeac1u },
    { 0xbd0561e6u, 0x211c70cfu },
    { 0x65dc9f50u, 0x3f63af83u },
    { 0xeab477d2u, 0x6415479cu },
    { 0x7f1b103cu, 0xdf1582b0u },
    { 0x2a96fb1au, 0x14a0f9e7u },
    { 0x66d22876u, 0x72fdf202u },
    { 0x4720771fu, 0x8ca81811u },
    { 0x230d977eu, 0xe2257159u },
    { 0x6e36aab0u, 0xd1bc52d9u },
    { 0x8cdb305fu, 0xdd2e1609u },
    { 0xb4d66a3au, 0x47469a4du },
    { 0xfc891b4au, 0x6a50df4du },
    { 0xaec6f024u, 0x5bd86d40u },
    { 0x616499c9u, 0xe25a7605u },
    { 0x3b1287ffu, 0xf52ddf5du },
    { 0x153e7c2au, 0x26a2c0bdu },
    { 0x26bb7dbdu, 0x2d1c9af0u },
    { 0xa8948c89u, 0x3b618676u },
    { 0x0316909eu, 0x3bbbe9eau },
    { 0xd4c28c2eu, 0x7c26847fu },
    { 0x2eae05cfu, 0x96d0cc5fu },
    { 0x482c9cbcu, 0x43435cc5u },
    { 0x254b0c4eu, 0x010c4759u },
    { 0x88daf401u, 0x6b4013efu },
    { 0x9c1caaf7u, 0x5e8766edu },
    { 0x519088f5u, 0x90fbbd11u },
    { 0x20203626u, 0xf3fe39c0u },
    { 0xdbf4a8b2u, 0xb0c4312du },
    { 0xf341e07au, 0x83f73f16u },
    { 0xa7abe1c2u, 0x9e1a8ef4u },
    { 0xbd628881u, 0xad1b72dbu },
    { 0x74e69a5du, 0x0dd27a65u },
    { 0xdef88334u, 0xe647cb8fu },
    { 0xf3aed0b6u, 0xc7ac1491u },
    { 0xae3a2b7fu, 0xdfe01893u },
    { 0x8f2c6ec8u, 0xcc4169a3u },
    { 0x65e7e423u, 0x6472f1a3u },
    { 0x64e50cadu, 0x66237a04u },
    { 0x7b45145cu, 0x1a81682cu },
    { 0x66836886u, 0xa260cd0bu },
    { 0x30cbc97du, 0x0fef7928u },
    { 0xfc132d0du, 0x113db17du },
    { 0x70ccec31u, 0x3571810au },
    { 0x1c2442f9u, 0x298cb3a5u },
    { 0x99c94309u, 0x570dc195u },
    { 0x1a358ca0u, 0x0d75985du },
    { 0x9118bb16u, 0x000f49c8u },
    { 0x895fd7b3u, 0x26b94c7fu },
    { 0xf2ee4e45u, 0x19f9919cu },
    { 0x9d1de2a0u, 0x5d158a2fu },
    { 0x1200339du, 0x068739fau },
    { 0x353c631cu, 0xdfd43f37u },
    { 0x6050914au, 0x9d33a01cu },
    { 0xa268aa87u, 0x2607679du },
    { 0xf4998d7cu, 0x4093f6deu },
    { 0x9a2ef80fu, 0x58ee8571u },
    { 0x7961fd92u, 0x5d39d0a8u },
    { 0x1d87cec3u, 0x1f7296abu },
    { 0x7cf20724u, 0xd953ee26u },
    { 0xfa529ba3u, 0xfe3bfadau },
    { 0x7afb2c68u, 0x774b15d7u },
    { 0x4fd58dbeu, 0x7bdc968bu },
    { 0x24e4e25au, 0x15fc899eu },
    { 0xbfeaa155u, 0x1a28f7b3u },
    { 0xbd87a865u, 0x57b6fb7eu },
    { 0x7a86f7a2u, 0x43c71b9au },
    { 0xb12aa1f6u, 0xd42fddbbu },
    { 0x842e7fc2u, 0x29540a6eu },
    { 0x3488f876u, 0x05e999f3u },
    { 0xf3b7a50du, 0xf373ca53u },
    { 0x5c9bcf35u, 0x873be078u },
    { 0xb0a844e5u, 0x2587be6bu },
    { 0xea057543u, 0x8b0d590bu },
    { 0xc215a82au, 0x06ec41adu },
    { 0x4c4f9b06u, 0x87322e25u },
    { 0xa49636a2u, 0xfa7f0eabu },
};
__attribute__((noinline)) u64 mix_ref(u64 x)
{
    for (int i = 0; i < 160; i++) {
        u64 v = ((u64)halves[i][0] << 32) | halves[i][1];
        switch (i % 3) {
        case 0: x = x ^ v; break;
        case 1: x = x + v; break;
        default: x = x - v; break;
        }
        x = x * 3 + salt;
    }
    return x;
}

__attribute__((noinline)) u64 shared(u64 a, u64 b)
{
    u64 r = 0;
    if (a & 1) r += 0x123456789abcdef1ULL;
    if (a & 2) r ^= 0x123456789abcdef1ULL;
    if (b > 0x123456789abcdef1ULL) r -= 0x123456789abcdef1ULL;
    return r + (a < 0x0fedcba987654321ULL ? 0x0fedcba987654321ULL : b);
}

__attribute__((noinline)) double poly(double x)
{
    return ((0.3183098861837907 * x + 2.718281828459045) * x -
            1.4142135623730951) * x + 3.141592653589793;
}

int main(void)
{
    int bad = 0;
    if (mix(5) != mix_ref(5) || mix(0xdeadbeefcafeULL) != mix_ref(0xdeadbeefcafeULL))
        bad |= 1;
    u64 s1 = shared(3, 0x123456789abcdef2ULL), s1w = 0;
    s1w += 0x123456789abcdef1ULL;
    s1w ^= 0x123456789abcdef1ULL;
    s1w -= 0x123456789abcdef1ULL;
    s1w += 0x0fedcba987654321ULL;
    if (s1 != s1w) bad |= 2;
    if (shared(0, 1) != 0x0fedcba987654321ULL) bad |= 4;
    double p = poly(2.0);
    /* 0.318..*8 + 2.718..*4 - 1.414..*2 + 3.14..: between 13.7 and 13.8 */
    if (!(p > 13.7 && p < 13.8)) bad |= 8;
    return bad ? bad : 42;
}
