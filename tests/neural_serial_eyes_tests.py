"""Compile/run actual private scheduling and option helpers; no device/workers/GPU."""
import json
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
VCVARS = Path(r"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat")
MAIN = r'''
} }
#include <cassert>
#include <vector>
int main(int argc,char** argv) {
    using namespace kotorvr::host;
    assert(argc==2);
    for (const auto* value : {L"",L"0",L"01",L"true",L"1 ",L"11",L"11111"}) {
        SetEnvironmentVariableW(L"KOTOR2VR_NEURAL_SERIAL_EYES",value);
        assert(!ReadSerialEyesOption());
    }
    SetEnvironmentVariableW(L"KOTOR2VR_NEURAL_SERIAL_EYES",nullptr);
    assert(!ReadSerialEyesOption());
    SetEnvironmentVariableW(L"KOTOR2VR_NEURAL_SERIAL_EYES",L"1");
    assert(ReadSerialEyesOption());
    for (const bool serial : {false,true}) {
        std::vector<unsigned> calls;
        auto submit=[&](unsigned eye) { calls.push_back(10+eye); };
        auto wait=[&](unsigned mask) { calls.push_back(20+mask); };
        RunEyeSchedule(serial,submit,wait);
        assert(calls==(serial ? std::vector<unsigned>{10,21,11,22}:std::vector<unsigned>{10,11,23}));
        // A wait failure (ACK error, fence failure, or timeout in the real
        // callback) must propagate; serial must never submit the right eye.
        calls.clear(); bool caught=false;
        try { RunEyeSchedule(serial,submit,[&](unsigned mask) { wait(mask); throw std::runtime_error("wait failed"); }); }
        catch (const std::runtime_error&) { caught=true; }
        assert(caught && calls==(serial ? std::vector<unsigned>{10,21}:std::vector<unsigned>{10,11,23}));
        for (unsigned fail_eye=0;fail_eye<2;++fail_eye) {
            calls.clear(); caught=false;
            try { RunEyeSchedule(serial,[&](unsigned eye) { submit(eye); if (eye==fail_eye) throw std::runtime_error("submit failed"); },wait); }
            catch (const std::runtime_error&) { caught=true; }
            assert(caught);
            assert(calls==(fail_eye==0 ? std::vector<unsigned>{10}:
                (serial ? std::vector<unsigned>{10,21,11}:std::vector<unsigned>{10,11})));
        }
    }
    NeuralTimingTrace trace;
    trace.path=std::filesystem::absolute(argv[1]);
    trace.Write(2040,2232,2040,2232,false,false);
    trace.serial_eyes=true;
    trace.Write(2040,2232,2040,2232,false,false);
}
'''

def main():
    source = (ROOT / "src/host64/src/neural_stereo_pipeline.cpp").read_text(encoding="utf-8")
    prefix, marker, _ = source.partition("void Check(HRESULT result,const char* stage)")
    assert marker
    with tempfile.TemporaryDirectory(prefix="kotor-serial-eyes-cpu-") as temp:
        folder = Path(temp)
        unit = folder / "serial_cpu.cpp"
        unit.write_text(prefix + MAIN, encoding="utf-8")
        executable = folder / "serial_cpu.exe"
        flags = ["cl.exe", "/nologo", "/std:c++20", "/EHsc", "/W4", "/WX",
                 "/DNOMINMAX", "/DWIN32_LEAN_AND_MEAN", "/I", str(ROOT / "src/host64/include"),
                 "/I", str(ROOT / "src/common"), str(unit), "/Fe:" + str(executable)]
        command = f'"{VCVARS}" >nul && ' + subprocess.list2cmdline(flags)
        env = {key.upper(): value for key, value in os.environ.items()}
        subprocess.run(f'cmd.exe /d /s /c "{command}"', cwd=folder, env=env, check=True)
        output = folder / "trace.jsonl"
        subprocess.run([str(executable), str(output)], cwd=folder, env=env, check=True)
        rows = [json.loads(line) for line in output.read_text().splitlines()]
        assert len(rows) == 2
        assert [row["serial_eyes"] for row in rows] == [False, True]
        assert rows[0]["schema"] == rows[1]["schema"] == 1
    print("PASS CPU: exact opt-in/default-off, actual serial/concurrent schedule, submit/wait failure propagation, trace mode; no GPU")

if __name__ == "__main__":
    main()
