// SPDX-License-Identifier: GPL-3.0+
#pragma once
#include <QString>

namespace AgentConsoleServer
{
	bool Start(const QString& directory, QString* error);
	void Stop();
	void ReportStartupError(const QString& error);
} // namespace AgentConsoleServer
