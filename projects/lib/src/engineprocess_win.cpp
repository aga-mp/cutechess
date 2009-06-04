/*
    This file is part of Cute Chess.

    Cute Chess is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    Cute Chess is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Cute Chess.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "engineprocess_win.h"
#include "pipereader_win.h"
#include <QDir>
#include <QTimerEvent>
#include <QtDebug>


EngineProcess::EngineProcess(QObject* parent)
	: QIODevice(parent),
	  m_started(false),
	  m_finished(false),
	  m_exitCode(0),
	  m_exitStatus(EngineProcess::NormalExit),
	  m_inWrite(INVALID_HANDLE_VALUE),
	  m_outRead(INVALID_HANDLE_VALUE),
	  m_reader(0)
{
}

EngineProcess::~EngineProcess()
{
	if (m_started)
	{
		qWarning("EngineProcess: Destroyed while process is still running.");
		kill();
		waitForFinished();
	}
	cleanup();
}

int EngineProcess::exitCode() const
{
	return (int)m_exitCode;
}

EngineProcess::ExitStatus EngineProcess::exitStatus() const
{
	return m_exitStatus;
}

qint64 EngineProcess::bytesAvailable() const
{
	qint64 n = QIODevice::bytesAvailable();

	if (!m_started)
		return n;
	return m_reader->bytesAvailable() + n;
}

bool EngineProcess::canReadLine() const
{
	if (!m_started)
		return QIODevice::canReadLine();
	return m_reader->canReadLine() || QIODevice::canReadLine();
}

void EngineProcess::killHandle(HANDLE* handle)
{
	if (*handle == INVALID_HANDLE_VALUE)
		return;
	CloseHandle(*handle);
	*handle = INVALID_HANDLE_VALUE;
}

void EngineProcess::cleanup()
{
	if (m_reader != 0)
	{
		if (m_reader->isRunning())
		{
			qWarning("EngineProcess: pipe reader was terminated");
			m_reader->terminate();
		}
		delete m_reader;
		m_reader = 0;
	}

	killHandle(&m_inWrite);
	killHandle(&m_outRead);

	killHandle(&m_processInfo.hProcess);
	killHandle(&m_processInfo.hThread);

	m_started = false;
}

void EngineProcess::close()
{
	if (!m_started)
		return;

	emit aboutToClose();
	kill();
	waitForFinished(-1);
	cleanup();
	QIODevice::close();
}

bool EngineProcess::isSequential() const
{
	return true;
}

void EngineProcess::setWorkingDirectory(const QString& dir)
{
	m_workDir = dir;
}

static QString quoteString(QString str)
{
	if (!str.contains(' '))
		return str;

	if (!str.startsWith('\"'))
		str.push_front('\"');
	if (!str.endsWith('\"'))
		str.push_back('\"');

	return str;
}

static QString commandLine(const QString& prog, const QStringList& args)
{
	QString cmd = QDir::toNativeSeparators(quoteString(prog));
	foreach (const QString& arg, args)
		cmd += ' ' + quoteString(arg);

	return cmd;
}

void EngineProcess::start(const QString& program,
			 const QStringList& arguments,
			 QIODevice::OpenMode mode)
{
	if (m_started)
		close();

	m_started = false;
	m_finished = false;
	m_exitCode = 0;
	m_exitStatus = NormalExit;

	// Temporary handles for the child process' end of the pipes
	HANDLE outWrite;
	HANDLE inRead;

	// Security attributes. Use the same one for both pipes.
	SECURITY_ATTRIBUTES saAttr;
	saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
	saAttr.bInheritHandle = TRUE;
	saAttr.lpSecurityDescriptor = NULL;

	CreatePipe(&m_outRead, &outWrite, &saAttr, 0);
	CreatePipe(&inRead, &m_inWrite, &saAttr, 0);

	STARTUPINFO startupInfo;
	ZeroMemory(&startupInfo, sizeof(startupInfo));
	startupInfo.cb = sizeof(startupInfo);
	startupInfo.hStdError = outWrite;
	startupInfo.hStdOutput = outWrite;
	startupInfo.hStdInput = inRead;
	startupInfo.dwFlags |= STARTF_USESTDHANDLES;

	// Call DuplicateHandle with a NULL target to get non-inheritable
	// handles for the parent process' ends of the pipes
	DuplicateHandle(GetCurrentProcess(),
			m_outRead,		// child's stdout read end
			GetCurrentProcess(),
			NULL,			// no target
			0,			// flags
			FALSE,			// not inheritable
			DUPLICATE_SAME_ACCESS);	// same handle access
	DuplicateHandle(GetCurrentProcess(),
			m_inWrite,		// child's stdin write end
			GetCurrentProcess(),
			NULL,			// no target
			0,			// flags
			FALSE,			// not inheritable
			DUPLICATE_SAME_ACCESS);	// same handle access

	BOOL ok = FALSE;
	QString cmd = commandLine(program, arguments);
	QString wdir = QDir::toNativeSeparators(m_workDir);
	ZeroMemory(&m_processInfo, sizeof(m_processInfo));

#ifdef UNICODE
	ok = CreateProcessW(NULL,
			    (WCHAR*)cmd.utf16(),
			    NULL,	// process attributes
			    NULL,	// thread attributes
			    TRUE,	// inherit handles
			    0,		// creation flags
			    NULL,	// environment
			    wdir.isEmpty() ? NULL : (WCHAR*)wdir.utf16(),
			    &startupInfo,
			    &m_processInfo);
#else // not UNICODE
	ok = CreateProcessA(NULL,
			    cmd.toLocal8Bit().data(),
			    NULL,	// process attributes
			    NULL,	// thread attributes
			    TRUE,	// inherit handles
			    0,		// creation flags
			    NULL,	// environment
			    wdir.isEmpty() ? NULL : wdir.toLocal8Bit().data(),
			    &startupInfo,
			    &m_processInfo);
#endif // not UNICODE

	m_started = (bool)ok;
	if (ok)
	{
		// Close the child process' ends of the pipes to make sure
		// that ReadFile and WriteFile will return when the child
		// terminates and closes its pipes
		killHandle(&outWrite);
		killHandle(&inRead);

		// Start reading input from the child
		m_reader = new PipeReader(m_outRead, this);
		connect(m_reader, SIGNAL(finished()), this, SLOT(onFinished()));
		connect(m_reader, SIGNAL(finished()), this, SIGNAL(readChannelFinished()));
		connect(m_reader, SIGNAL(readyRead()), this, SIGNAL(readyRead()));
		m_reader->start();

		// Make QIODevice aware that the device is now open
		QIODevice::open(mode);
	}
	else
		cleanup();
}

void EngineProcess::start(const QString& program,
			 QIODevice::OpenMode mode)
{
	start(program, QStringList(), mode);
}

void EngineProcess::kill()
{
	if (m_started)
		TerminateProcess(m_processInfo.hProcess, 0xf291);
}

void EngineProcess::onFinished()
{
	if (!m_started || m_finished)
		return;

	if (GetExitCodeProcess(m_processInfo.hProcess, &m_exitCode)
	&&  m_exitCode != STILL_ACTIVE)
	{
		m_finished = true;
		m_exitStatus = NormalExit;
		if (m_exitCode != 0)
			m_exitStatus = CrashExit;

		emit finished((int)m_exitCode, m_exitStatus);
	}
}

bool EngineProcess::waitForFinished(int msecs)
{
	if (!m_started)
		return true;

	DWORD dwWait;
	if (msecs == -1)
		dwWait = INFINITE;
	else
		dwWait = msecs;

	DWORD ret = WaitForSingleObject(m_processInfo.hProcess, dwWait);
	if (ret == WAIT_OBJECT_0)
	{
		onFinished();

		// The blocking ReadFile call in the pipe reader should
		// return now that the pipes are closed. But if it doesn't
		// happen, the pipe reader will be terminated violently
		// after the timeout.
		m_reader->wait(10000);
		cleanup();

		return true;
	}
	return false;
}

bool EngineProcess::waitForStarted(int msecs)
{
	// Don't wait here because CreateProcess already did the waiting
	Q_UNUSED(msecs);
	return m_started;
}

QString EngineProcess::workingDirectory() const
{
	return m_workDir;
}

qint64 EngineProcess::readData(char* data, qint64 maxSize)
{
	if (!m_started)
		return -1;

	return m_reader->readData(data, maxSize);
}

qint64 EngineProcess::writeData(const char* data, qint64 maxSize)
{
	if (!m_started)
		return -1;

	DWORD dwWritten = 0;
	if (!WriteFile(m_inWrite, data, (DWORD)maxSize, &dwWritten, 0))
		return -1;
	return (qint64)dwWritten;
}
