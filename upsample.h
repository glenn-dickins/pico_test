// Create a set of samples at twice the input rate using a fixed filter
// This filter also includes a pre-emphasis to compensate for amp
// The filter has 20 taps, so this requires that there are 19 previous samples before *in
// The input should already be scaled down by 8 bits for the math and headroom
// The filter uses effective 6 bit coefficients
// Allow the output to be written in interleaved format
// 
// W = fir1(63, 0.4751, 'low', kbdwin(64, 5), 'noscale');
// W = filter(1.3,[1 0 .3],W);
// reshape(round(W(13:end-10)*64),2,[])
//
//    -1     1    -1     2    -3     4    -5     6    -8    16    31   -15     7    -3     1     0     0     1    -1     1    -1
//     1    -1     1    -1     0     0    -1     3    -7    38     2    -7     7    -5     4    -3     3    -2     1    -1     0

#define FILTER2X_TAPS 21


void filter2x(int32_t *in, int32_t *out, int n, int out_stride = 1)
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

        if (z1 >  0x1FFFFFFF) z1 =  0x1FFFFFFF;
        if (z1 < -0x20000000) z1 = -0x20000000; 
        *out = *in;
        out += out_stride;

        if (z2 >  0x1FFFFFFF) z2 =  0x1FFFFFFF;
        if (z2 < -0x20000000) z2 = -0x20000000;
        *out = 0;
        out += out_stride;

        in++;
    }
}