extern "C" __declspec(dllimport) int leafValue();

extern "C" __declspec(dllexport) int lateValue()
{
    return leafValue() + 1;
}
