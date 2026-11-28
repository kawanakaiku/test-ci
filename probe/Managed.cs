using System;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Runtime.CompilerServices;

class Managed
{
    static volatile int signedLeft, signedRight;
    static volatile uint unsignedLeft, unsignedRight;
    static long wideLeft, wideRight;
    static string selectedCase;

    [MethodImpl(MethodImplOptions.NoInlining)]
    static int Signed32() { return signedLeft / signedRight; }

    [MethodImpl(MethodImplOptions.NoInlining)]
    static uint Unsigned32() { return unsignedLeft / unsignedRight; }

    [MethodImpl(MethodImplOptions.NoInlining)]
    static long Signed64() { return wideLeft / wideRight; }

    static void Run<T>(TextWriter output, string name, Func<T> divide)
    {
        if (selectedCase != null && selectedCase != name)
            return;

        output.Write(name + " ");
        output.Flush();
        try
        {
            T quotient = divide();
            output.WriteLine("quotient=" + quotient);
        }
        catch (Exception ex)
        {
            StackFrame frame = new StackTrace(ex, false).GetFrame(0);
            output.WriteLine("{0} method={1} il={2}", ex.GetType().Name,
                frame.GetMethod().Name, frame.GetILOffset());
        }
        output.Flush();
    }

    static void Main(string[] args)
    {
        if (args.Length != 4 && args.Length != 5)
            throw new ArgumentException("usage: Managed output-file numerator divisor zero [case]");
        selectedCase = args.Length == 5 ? args[4] : null;

        using (TextWriter output = new StreamWriter(args[0]))
        {
            output.WriteLine("pointer-size={0} clr={1}", IntPtr.Size, Environment.Version);
            signedLeft = int.Parse(args[1], CultureInfo.InvariantCulture);
            signedRight = int.Parse(args[2], CultureInfo.InvariantCulture);
            unsignedLeft = (uint)signedLeft;
            unsignedRight = (uint)signedRight;
            wideLeft = signedLeft;
            wideRight = signedRight;
            Run(output, "idiv32/normal", Signed32);
            Run(output, "div32/normal", Unsigned32);
            Run(output, "idiv64/normal", Signed64);

            signedRight = int.Parse(args[3], CultureInfo.InvariantCulture);
            unsignedRight = (uint)signedRight;
            wideRight = signedRight;
            Run(output, "idiv32/zero", Signed32);
            Run(output, "div32/zero", Unsigned32);
            Run(output, "idiv64/zero", Signed64);

            signedLeft = int.MinValue;
            wideLeft = long.MinValue;
            signedRight = -1;
            wideRight = signedRight;
            Run(output, "idiv32/min/-1", Signed32);
            Run(output, "idiv64/min/-1", Signed64);
            output.WriteLine("done");
        }
    }
}
