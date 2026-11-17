/*
 * ClassroomSignInDialog.cpp - ikeback name + seat sign-in dialog
 *
 * This file is part of the Ikeback classroom modification.
 */

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "ClassroomSignInDialog.h"


ClassroomSignInDialog::ClassroomSignInDialog( const QStringList& classNames,
											  const QString& currentClassName,
											  const QJsonArray& classes,
											  QWidget* parent ) :
	QDialog( parent ),
	m_classes( classes ),
	m_classCombo( new QComboBox( this ) ),
	m_studentCombo( new QComboBox( this ) ),
	m_nameEdit( new QLineEdit( this ) ),
	m_seatEdit( new QLineEdit( this ) ),
	m_buttons( new QDialogButtonBox( QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this ) )
{
	setWindowTitle( tr( "学生登录" ) );
	setMinimumWidth( 400 );

	m_classCombo->addItems( classNames );
	if( classNames.isEmpty() ) { m_classCombo->addItem( tr( "（名册中暂无班级）" ) ); }
	const auto index = m_classCombo->findText( currentClassName );
	if( index >= 0 ) { m_classCombo->setCurrentIndex( index ); }

	m_nameEdit->setPlaceholderText( tr( "学生姓名" ) );
	m_seatEdit->setPlaceholderText( tr( "座位号" ) );

	auto* form = new QFormLayout;
	form->addRow( tr( "班级：" ), m_classCombo );
	form->addRow( tr( "从名单选择：" ), m_studentCombo );
	form->addRow( tr( "姓名：" ), m_nameEdit );
	form->addRow( tr( "座位号：" ), m_seatEdit );

	auto* note = new QLabel( tr( "仅按姓名和座位号登录，不需要也不保存 Windows 密码。" ), this );
	note->setWordWrap( true );

	auto* layout = new QVBoxLayout( this );
	layout->addLayout( form );
	layout->addWidget( note );
	layout->addWidget( m_buttons );

	connect( m_classCombo, &QComboBox::currentTextChanged, this,
			 &ClassroomSignInDialog::refreshRosterStudents );
	connect( m_studentCombo, QOverload<int>::of( &QComboBox::currentIndexChanged ), this,
			 &ClassroomSignInDialog::selectRosterStudent );
	connect( m_nameEdit, &QLineEdit::textChanged, this, &ClassroomSignInDialog::updateOkButton );
	connect( m_seatEdit, &QLineEdit::textChanged, this, &ClassroomSignInDialog::updateOkButton );
	connect( m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept );
	connect( m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject );
	refreshRosterStudents();
	updateOkButton();
}


void ClassroomSignInDialog::refreshRosterStudents()
{
	m_studentCombo->blockSignals( true );
	m_studentCombo->clear();
	m_studentCombo->addItem( tr( "（手动输入）" ), QStringList{} );
	const auto className = m_classCombo->currentText();
	for( const auto& classValue : m_classes )
	{
		const auto object = classValue.toObject();
		if( object.value( QStringLiteral("name") ).toString() != className ) { continue; }
		for( const auto& studentValue : object.value( QStringLiteral("students") ).toArray() )
		{
			const auto student = studentValue.toObject();
			const auto seat = student.value( QStringLiteral("seat") ).toString();
			const auto name = student.value( QStringLiteral("name") ).toString();
			if( seat.isEmpty() && name.isEmpty() ) { continue; }
			m_studentCombo->addItem( QStringLiteral("%1 - %2").arg( seat, name ), QStringList{ seat, name } );
		}
	}
	m_studentCombo->blockSignals( false );
}


void ClassroomSignInDialog::selectRosterStudent( int index )
{
	if( index <= 0 ) { return; }
	const auto values = m_studentCombo->itemData( index ).toStringList();
	if( values.size() < 2 ) { return; }
	m_seatEdit->setText( values.at( 0 ) );
	m_nameEdit->setText( values.at( 1 ) );
}


void ClassroomSignInDialog::updateOkButton()
{
	auto* ok = m_buttons->button( QDialogButtonBox::Ok );
	if( ok )
	{
		ok->setEnabled( m_nameEdit->text().trimmed().isEmpty() == false &&
						m_seatEdit->text().trimmed().isEmpty() == false );
	}
}


QString ClassroomSignInDialog::className() const
{
	return m_classCombo->currentText();
}


QString ClassroomSignInDialog::studentName() const
{
	return m_nameEdit->text().trimmed();
}


QString ClassroomSignInDialog::seat() const
{
	return m_seatEdit->text().trimmed();
}
