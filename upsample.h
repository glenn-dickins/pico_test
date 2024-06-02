// Create a set of samples at twice the input rate using a fixed filter
// This filter also includes a pre-emphasis to compensate for amp
// The filter has 20 taps, so this requires that there are 19 previous samples before *in
// The input should already be scaled down by 8 bits for the math and headroom
// The filter uses effective 6 bit coefficients
// Allow the output to be written in interleaved format
// 
// W = 2*fir1(63, 0.4751, 'low', kbdwin(64, 5), 'noscale');
// W = filter(1.3,[1 0 .3],W);
//
// A = reshape(round(W(13:end-10)*32),2,[])     % Fortunate pick is already balanced (rows sum to 32)
//    -1     1    -1     2    -3     4    -5     6    -8    16    31   -15     7    -3     1     0     0     1    -1     1    -1
//     1    -1     1    -1     0     0    -1     3    -7    38     2    -7     7    -5     4    -3     3    -2     1    -1     0
//
//
// B = reshape(W1(11:end-8),2,[])*128
// B(1,11)=B(1,11)+1;                           % Balance for unity gain
// B(2,16)=B(2,16)+1;
// B(2,1 )=B(2,1 )+1;
//
//      1    -2     4    -6     8   -11    14   -18    23   -33    66   124   -61    28   -13     5    -1    -1     2    -3     3    -2     1
//     -1     2    -3     3    -2     1     1    -5    11   -27   152    10   -29    27   -22    18   -14    11    -8     5    -3     2    -1
//
// This second variant needs more actual multiplies and is about 30-40% more load.  
// Detail on this in Matlab in daes67/design/Investigations.m
//


#define FILTER2X_TAPS 23

void filter2x_b(int32_t *in, int32_t *out, int n, int out_stride = 1)
{
    for (int i = 0; i < n; i++)
    {
        int32_t *p = in;

        int32_t z1 =   1 * *p;
        int32_t z2 =  -1 * *p;
        p--;
        
        z1 =    z1 +  -2 * *p;
        z2 =    z2 +   2 * *p;
        p--;

        z1 =    z1 +   4 * *p;
//        z2 =    z2 +  -3 * *p;
        z2 = z2 + __SMULBB(5, *p);

        p--;

        z1 =    z1 +  -6 * *p;
        z2 =    z2 +   3 * *p;
        p--;

        z1 =    z1 +   8 * *p;
        z2 =    z2 +  -2 * *p;
        p--;

        z1 =    z1 + -11 * *p;
        z2 =    z2 +   1 * *p;
        p--;
        
        z1 =    z1 +  14 * *p;
        z2 =    z2 +   1 * *p;
        p--;

        z1 =    z1 + -18 * *p;
        z2 =    z2 +  -5 * *p;
        p--;

        z1 =    z1 +  23 * *p;
        z2 =    z2 +  11 * *p;
        p--;

        z1 =    z1 + -33 * *p;
        z2 =    z2 + -27 * *p;
        p--;

        z1 =    z1 +  66 * *p;
        z2 =    z2 + 152 * *p;
        p--;

        z1 =    z1 + 124 * *p;
        z2 =    z2 +  10 * *p;
        p--;

        z1 =    z1 + -61 * *p;
        z2 =    z2 + -29 * *p;
        p--;

        z1 =    z1 +  28 * *p;
        z2 =    z2 +  27 * *p;
        p--;

        z1 =    z1 + -13 * *p;
        z2 =    z2 + -22 * *p;
        p--;

        z1 =    z1 +   5 * *p;
        z2 =    z2 +  18 * *p;
        p--;

        z1 =    z1 +  -1 * *p;
        z2 =    z2 + -14 * *p;
        p--;

        z1 =    z1 +  -1 * *p;
        z2 =    z2 +  11 * *p;
        p--;

        z1 =    z1 +   2 * *p;
        z2 =    z2 +  -8 * *p;
        p--;

        z1 =    z1 +  -3 * *p;
        z2 =    z2 +   5 * *p;
        p--;

        z1 =    z1 +   3 * *p;
        z2 =    z2 +  -3 * *p;
        p--;

        z1 =    z1 +  -2 * *p;
        z2 =    z2 +   2 * *p;
        p--;

        z1 =    z1 +   1 * *p;
        z2 =    z2 +  -1 * *p;
        p--;

        if (z1 >  0x3FFFFFFF) z1 =  0x3FFFFFFF;
        if (z1 < -0x40000000) z1 = -0x40000000; 
        *out = z1<<1;
        out += out_stride;

        if (z2 >  0x3FFFFFFF) z2 =  0x3FFFFFFF;
        if (z2 < -0x40000000) z2 = -0x40000000;
        *out = z2<<1;
        out += out_stride;

        in++;
    }
}

void filter2x_a(int32_t *in, int32_t *out, int n, int out_stride = 1)
{
    for (int i = 0; i < n; i++)
    {
        int32_t *p = in;
        int32_t z1 =  -1 * *p;
        int32_t z2 =   1 * *p;
        p--;
        
        z1 =    z1 +   1 * *p;
        z2 =    z2 +  -1 * *p;
        p--;

        z1 =    z1 +  -1 * *p;
        z2 =    z2 +   1 * *p;
        p--;

        z1 =    z1 +   2 * *p;
        z2 =    z2 +  -1 * *p;
        p--;

        z1 =    z1 +  -3 * *p;
//      z2 =    z2 +   0 * *p;
        p--;

        z1 =    z1 +   4 * *p;
//      z2 =    z2 +   0 * *p;
        p--;
        
        z1 =    z1 +  -5 * *p;
        z2 =    z2 +  -1 * *p;
        p--;

        z1 =    z1 +   6 * *p;
        z2 =    z2 +   3 * *p;
        p--;

        z1 =    z1 +  -8 * *p;
        z2 =    z2 +  -7 * *p;
        p--;

        z1 =    z1 +  16 * *p;
        z2 =    z2 +  38 * *p;
        p--;

        z1 =    z1 +  31 * *p;
        z2 =    z2 +   2 * *p;
        p--;

        z1 =    z1 + -15 * *p;
        z2 =    z2 +  -7 * *p;
        p--;

        z1 =    z1 +   7 * *p;
        z2 =    z2 +   7 * *p;
        p--;

        z1 =    z1 +  -3 * *p;
        z2 =    z2 +  -5 * *p;
        p--;

        z1 =    z1 +   1 * *p;
        z2 =    z2 +   4 * *p;
        p--;

//      z1 =    z1 +   0 * *p;
        z2 =    z2 +  -3 * *p;
        p--;

//      z1 =    z1 +   0 * *p;
        z2 =    z2 +   3 * *p;
        p--;

        z1 =    z1 +   1 * *p;
        z2 =    z2 +  -2 * *p;
        p--;

        z1 =    z1 +  -1 * *p;
        z2 =    z2 +   1 * *p;
        p--;

        z1 =    z1 +   1 * *p;
        z2 =    z2 +  -1 * *p;
        p--;

        z1 =    z1 +  -1 * *p;
//      z2 =    z2 +   0 * *p;
        p--;

        if (z1 >  0x0FFFFFFF) z1 =  0x0FFFFFFF;
        if (z1 < -0x10000000) z1 = -0x10000000; 
        *out = z1<<3;
        out += out_stride;

        if (z2 >  0x0FFFFFFF) z2 =  0x0FFFFFFF;
        if (z2 < -0x10000000) z2 = -0x10000000;
        *out = z2<<3;
        out += out_stride;

        in++;
    }
}