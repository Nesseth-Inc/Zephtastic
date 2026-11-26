"""Own one offline compiler/test process group; never signal the caller's group."""
import os,signal,subprocess,time

class TerminationRequested(BaseException):
    """A dedicated qualification driver received an external SIGTERM."""

def install_termination_handler():
    """Use only in these one-shot drivers, before launching owned children."""
    def terminate(signum,frame):
        raise TerminationRequested(signum)
    return signal.signal(signal.SIGTERM,terminate)

def group_gone(pgid,seconds=.5):
    deadline=time.monotonic()+seconds
    while True:
        try:os.killpg(pgid,0)
        except ProcessLookupError:return True
        if time.monotonic()>=deadline:return False
        time.sleep(.02)

def run_group(argv,*,timeout,stdout,env=None,cwd=None):
    """Fail on timeout, interruption, nonzero exit or surviving descendants.

    An unreaped zombie group also fails closed. No global subreaper or host
    setting is changed. Caller must stop qualification if this function raises.
    """
    process=None;cleanup={'leader_reaped':False,'group_gone':False}
    try:
        process=subprocess.Popen(argv,stdout=stdout,stderr=subprocess.STDOUT,
                                 env=env,cwd=cwd,start_new_session=True)
        result=process.wait(timeout=timeout)
        cleanup['leader_reaped']=True
        cleanup['group_gone']=group_gone(process.pid)
        if not cleanup['group_gone']:raise RuntimeError('Owned descendants still present')
        if result:raise subprocess.CalledProcessError(result,argv)
        return subprocess.CompletedProcess(argv,result)
    except BaseException as error:
        if process is not None:
            try:os.killpg(process.pid,signal.SIGKILL)
            except ProcessLookupError:pass
            except BaseException as failure:cleanup['signal_error_type']=type(failure).__name__
            try:process.wait(timeout=1);cleanup['leader_reaped']=True
            except BaseException as failure:cleanup['reap_error_type']=type(failure).__name__
            cleanup['group_gone']=group_gone(process.pid)
        error.owned_process_cleanup=cleanup
        raise
