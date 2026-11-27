#pragma once

#include <QColor>

class QWidget;

namespace Classroom
{

class Theme
{
public:
	enum class ColorRole
	{
		Background,
		Surface,
		SurfaceAlternate,
		Text,
		Muted,
		Border,
		Primary,
		PrimaryHover,
		Secondary,
		Accent,
		Success,
		Warning,
		Danger,
		Disabled
	};

	static void apply( QWidget* widget );
	static QColor color( ColorRole role );
};

}
