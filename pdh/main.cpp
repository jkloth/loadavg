#include <windows.h>
#include <stdio.h>
#include <conio.h>

#define LOADAVG_USE_REG
//#define LOADAVG_USE_PDH

#if defined(LOADAVG_USE_PDH)
#include <pdh.h>
#include <pdhmsg.h>
#pragma comment(lib, "pdh.lib")
#elif defined(LOADAVG_USE_REG)
#include <winperf.h>
#pragma comment(lib, "advapi32.lib")
#else
#error one of LOADAVG_USE_* must be defined
#endif

/* The load average is calculated using an exponential decaying average.
 *    S1 = a * Y + (1 - a) * S0
 *     a = (1/2) ** (t / t1/2)
 *  t1/2 = T * ln(2)
 *     
 * where:
 *   - a is the smoothing factor,
 *   - Y is the current observation,
 *   - S0 is the previous smoothed statistic,
 *   - S1 is the simple weighted average of Y and S0
 *   - t is the sample rate (5sec on Linux)
 *   - t1/2 is the half-life
 *   - T is the mean lifetime (1min, 5min, 15min on Linux)
 *
 * >>> import decimal
 * >>> loadavg_sample_rate = 5
 * >>> def calc_factor(T):
 * ...     with decimal.localcontext() as ctx:
 * ...         exp = loadavg_sample_rate / (T * ctx.ln2(2))
 * ...         return ctx.power(2, -exp)
 * ...
 * >>> defines = ('#define LOADAVG_FACTOR_1F   {T1}\n'
 * ...            '#define LOADAVG_FACTOR_5F   {T5}\n'
 * ...            '#define LOADAVG_FACTOR_15F  {T15}\n'
 * ...            '#define LOADAVG_SAMPLE_RATE {t}\n')
 * >>> print(defines.format(T1=calc_factor(60),
 * ...                      T5=calc_factor(300),
 * ...                      T15=calc_factor(900),
 * ...                      t=loadavg_sample_rate))
 */
#define LOADAVG_FACTOR_1F   0.9200444146293232478931553241
#define LOADAVG_FACTOR_5F   0.9834714538216174894737477501
#define LOADAVG_FACTOR_15F  0.9944598480048967508795473395
#define LOADAVG_SAMPLE_RATE 5

typedef struct {
	double average[3];
	DWORD cbPerfData;
	PPERF_DATA_BLOCK pPerfData;
} LOADAVG_DATA, *PLOADAVG_DATA;


static inline double
calc_loadf(double load, double exp, double active)
{
	return load * exp + active * (1.0 - exp);
}

#define CALC_LOAD(n, f, r) (n) = calc_loadf((n), (f), (r))

static inline void
calc_load(PLOADAVG_DATA pLoadData, DWORD dwRunning)
{
	wprintf(L"(%d)\n", dwRunning);
	CALC_LOAD(pLoadData->average[0], LOADAVG_FACTOR_1F, dwRunning);
	CALC_LOAD(pLoadData->average[1], LOADAVG_FACTOR_5F, dwRunning);
	CALC_LOAD(pLoadData->average[2], LOADAVG_FACTOR_15F, dwRunning);
}

#ifdef LOADAVG_USE_PDH
VOID CALLBACK CalculateLoadPdh(
	PTP_CALLBACK_INSTANCE Instance,
	PVOID Context,
	PTP_TIMER Timer)
{
	PLOADAVG_DATA pLoadData = (PLOADAVG_DATA)Context;
	PDH_STATUS pdhStatus;
	HQUERY hQuery = NULL;
	HCOUNTER hCounter;
	PDH_RAW_COUNTER pdhValue;
	WCHAR *szCounterPath = L"\\System\\Processor Queue Length";

	UNREFERENCED_PARAMETER(Instance);
	UNREFERENCED_PARAMETER(Context);
	UNREFERENCED_PARAMETER(Timer);

	// Create a query.
	pdhStatus = PdhOpenQueryW(NULL, NULL, &hQuery);
	if (pdhStatus != ERROR_SUCCESS)
	{
		wprintf(L"\nPdhOpenQuery failed with status 0x%x.", pdhStatus);
		goto done;
	}

	// Add the selected counter to the query.
	pdhStatus = PdhAddEnglishCounterW(hQuery, szCounterPath, 0, &hCounter);
	if (pdhStatus != ERROR_SUCCESS) {
		wprintf(L"\nPdhAddCounter failed with status 0x%x.", pdhStatus);
		goto done;
	}

	pdhStatus = PdhCollectQueryData(hQuery);
	if (pdhStatus != ERROR_SUCCESS) {
		wprintf(L"\nPdhCollectQueryData failed with status 0x%x.", pdhStatus);
		goto done;
	}

	pdhStatus = PdhGetRawCounterValue(hCounter, NULL, &pdhValue);
	if (pdhStatus != ERROR_SUCCESS) {
		wprintf(L"\nPdhGetFormattedCounterValue failed with status 0x%x.", pdhStatus);
		goto done;
	}

	// Our counter is known to be PERF_COUNTER_RAWCOUNT (32-bit unsigned int)
	calc_load(pLoadData, (DWORD)pdhValue.FirstValue);

done:
	if (hQuery != NULL) {
		PdhCloseQuery(hQuery);
	}
}
#define CalculateLoad CalculateLoadPdh
#endif // LOADAVG_USE_PDH

#ifdef LOADAVG_USE_REG
VOID CALLBACK CalculateLoadReg(
	PTP_CALLBACK_INSTANCE Instance,
	PVOID Context,
	PTP_TIMER Timer)
{
	PLOADAVG_DATA pLoadData = (PLOADAVG_DATA)Context;
	PPERF_DATA_BLOCK pPerfData = pLoadData->pPerfData;
	LSTATUS status = ERROR_SUCCESS;
	PPERF_OBJECT_TYPE pObject;
	PPERF_COUNTER_DEFINITION pCounter;
	PBYTE pData;

	UNREFERENCED_PARAMETER(Instance);
	UNREFERENCED_PARAMETER(Timer);

	while (1) {
		PPERF_DATA_BLOCK pTemp;
		DWORD cbData = pLoadData->cbPerfData;
		/* get the 'System' object */
		status = RegQueryValueExW(HKEY_PERFORMANCE_DATA, L"2", NULL, NULL,
								  (LPBYTE)pPerfData, &cbData);
		if (status != ERROR_MORE_DATA)
			break;
		cbData = pLoadData->cbPerfData * 2;
		pTemp = (PPERF_DATA_BLOCK)realloc(pPerfData, cbData);
		if (pTemp == NULL) {
			wprintf(L"realloc failed.\n");
			goto done;
		}
		pLoadData->cbPerfData = cbData;
		pLoadData->pPerfData = pPerfData = pTemp;
	}
	if (status != ERROR_SUCCESS) {
		wprintf(L"RegQueryValueEx failed with status 0x%x.\n", status);
		goto done;
	}

	pObject = (PPERF_OBJECT_TYPE)
		((PBYTE)pPerfData + pPerfData->HeaderLength);
	pCounter = (PPERF_COUNTER_DEFINITION)
		((PBYTE)pObject + pObject->HeaderLength);
	pData = (PBYTE)pObject + pObject->DefinitionLength;
	/* find the 'Processor Queue Length' counter (index=44) */
	while ((INT_PTR)pCounter < (INT_PTR)pData) {
		if (pCounter->CounterNameTitleIndex == 44) {
			// Our counter is known to be PERF_COUNTER_RAWCOUNT (32-bit unsigned int)
			calc_load(pLoadData, *((PDWORD)(pData + pCounter->CounterOffset)));
			break;
		}
		pCounter++;
	}

done:
	RegCloseKey(HKEY_PERFORMANCE_DATA);
}
#define CalculateLoad CalculateLoadReg
#endif // LOADAVG_USE_REG


int wmain(void)
{
	const char spinchar[] = { '|', '/', '-', '\\', '|', '/', '-', '\\' };
	int spinpos = 0;

	LOADAVG_DATA LoadData = { { 0, 0, 0} };
	PTP_TIMER pTimer;
	FILETIME ft;
#ifdef LOADAVG_USE_REG
	PPERF_DATA_BLOCK pPerfData;
	// An initial allocation >944 should make it that RegQueryValueEx
	// only needs to be called once for the 'System' performance data.
	pPerfData = (PPERF_DATA_BLOCK)malloc(1024);
	if (pPerfData == NULL) {
		wprintf(L"malloc failed\n");
		goto done;
	}
	LoadData.cbPerfData = 1024;
	LoadData.pPerfData = pPerfData;
#endif

	/* create the thread pool timer thread */
	pTimer = CreateThreadpoolTimer(CalculateLoad, &LoadData, NULL);
	if (pTimer == NULL) {
		wprintf(L"CreateThreadpoolTimer failed: %d\n", GetLastError());
		goto done;
	}
	/* set timer to fire every 5 seconds (also runs immediately) */
	ft.dwHighDateTime = ft.dwLowDateTime = 0;
	SetThreadpoolTimer(pTimer, &ft, LOADAVG_SAMPLE_RATE * 1000, 0);

	while (!_kbhit())
	{
		wprintf(L"%c\t%2.2f %2.2f %2.2f\r", spinchar[spinpos],
			LoadData.average[0], LoadData.average[1], LoadData.average[2]);
		spinpos = (spinpos + 1) % 8;
		Sleep(100);
	}
	wprintf(L" \n");	/* clear spinner */
	(void) _getch();	/* discard keystroke */

	SetThreadpoolTimer(pTimer, NULL, 0, 0);
	WaitForThreadpoolTimerCallbacks(pTimer, TRUE);
	CloseThreadpoolTimer(pTimer);

	wprintf(L"Press any key to exit.\n");
	while (!_kbhit()) Sleep(250);

done:
#ifdef LOADAVG_USE_REG
	if (pPerfData) {
		free(pPerfData);
	}
#endif
	return 0;
}

