$ErrorActionPreference = 'Stop'
$repositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
. (Join-Path $repositoryRoot 'local-config.ps1')
$env:PYTHONUTF8 = '1'
# Use the environment script supplied by the user's existing IDF installation.
. $IdfEnvironmentScript
$env:IDF_CCACHE_ENABLE = '0'
# Preserve the initialized tool PATH for Python and child build tools.
$runner = "import os,runpy,sys; os.environ['PATH']=sys.argv.pop(1)+os.pathsep+os.environ['PATH']; sys.argv.pop(0); sys.path.insert(0,os.path.dirname(sys.argv[0])); runpy.run_path(sys.argv[0],run_name='__main__')"
& (Join-Path $env:IDF_PYTHON_ENV_PATH 'Scripts\python.exe') -c $runner $env:PATH (Join-Path $env:IDF_PATH 'tools\idf.py') @args
exit $LASTEXITCODE
