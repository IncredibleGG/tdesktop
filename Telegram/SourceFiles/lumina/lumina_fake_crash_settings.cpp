/*
This file is part of LuminaGram,
a fork of Telegram Desktop.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "lumina/lumina_fake_crash_settings.h"

#include "base/openssl_help.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "lang/lang_keys.h"
#include "lumina/lumina_fake_crash.h"
#include "lumina/lumina_locale.h"
#include "main/main_domain.h"
#include "settings/settings_common.h"
#include "storage/storage_domain.h"
#include "ui/boxes/confirm_box.h"
#include "ui/layers/generic_box.h"
#include "ui/vertical_list.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/wrap/slide_wrap.h"
#include "ui/wrap/vertical_layout.h"
#include "window/window_session_controller.h"

#include "styles/style_layers.h"
#include "styles/style_settings.h"
#include "styles/style_widgets.h"

namespace Lumina {
namespace {

// Long enough for a passphrase, short enough that the row label stays honest.
constexpr auto kCodeMaxLength = 64;

// Every label, toggle and paragraph on the block is recomputed from the store
// on any change to it, and on any change to the local passcode - the second
// one matters because two of the warnings below are about the passcode, and
// the passcode is edited on a completely different page. LangChanges() is
// merged in because every text they produce is itself a translated string.
[[nodiscard]] rpl::producer<> Changes() {
	return rpl::merge(
		FakeCrashChanges(),
		Core::App().domain().local().localPasscodeChanged(),
		LangChanges());
}

[[nodiscard]] rpl::producer<QString> LabelValue(Fn<QString()> compute) {
	return rpl::single(
		rpl::empty
	) | rpl::then(
		Changes()
	) | rpl::map([compute = std::move(compute)] {
		return compute();
	});
}

[[nodiscard]] rpl::producer<bool> FlagValue(Fn<bool()> compute) {
	return rpl::single(
		rpl::empty
	) | rpl::then(
		Changes()
	) | rpl::map([compute = std::move(compute)] {
		return compute();
	});
}

[[nodiscard]] bool HasLocalPasscode() {
	const auto &domain = Core::App().domain();
	return domain.started() && domain.local().hasLocalPasscode();
}

// Answers done(true) when `code` would unlock the app, on the main thread and
// only while `guard` is alive.
//
// Since 7.3 the passcode check is a memory-hard derivation that upstream never
// runs on the main thread: Storage::Domain::checkPasscode() takes a prepared
// PasscodeDerivation, and the derivation itself runs on a worker through
// Storage::DeriveOnWorker() - the same way the lock screen and the passcode
// settings page check it now. So this answers later, not at once.
//
// checkPasscode() asserts on key material that only exists once the domain
// has started (Expects(_localKey) in storage/storage_domain.cpp), and there is
// nothing to collide with when no passcode lock is set at all - hence both
// guards, not just the obvious one, checked again when the verdict arrives.
// hasLocalPasscode() now means "the app lock is armed": a passcode kept only
// for the wallet never shows the lock screen, so it cannot collide with a
// duress code typed there either.
void CheckCodeIsLocalPasscode(
		const QString &code,
		not_null<QObject*> guard,
		Fn<void(bool)> done) {
	if (code.isEmpty() || !HasLocalPasscode()) {
		done(false);
		return;
	}
	// Same conversion the lock screen applies to what the user typed
	// (window/window_lock_widgets.cpp, TryPasscode), cleansed the same way.
	auto utf8 = code.toUtf8();
	const auto cleanse = gsl::finally([&] {
		if (!utf8.isEmpty()) {
			OPENSSL_cleanse(utf8.data(), utf8.size());
		}
	});
	const auto &local = Core::App().domain().local();
	Storage::DeriveOnWorker(
		local.prepareOpen(utf8),
		crl::guard(guard, [done](Storage::PasscodeDerivation &&derived) {
			done(HasLocalPasscode()
				&& Core::App().domain().local().checkPasscode(
					std::move(derived)));
		}));
}

void AddToggleRow(
		not_null<Ui::VerticalLayout*> container,
		rpl::producer<QString> label,
		Fn<bool()> checked,
		Fn<void(bool)> save) {
	const auto button = container->add(object_ptr<Ui::SettingsButton>(
		container,
		std::move(label),
		st::settingsButtonNoIcon
	))->toggleOn(FlagValue(std::move(checked)));
	button->toggledChanges(
	) | rpl::on_next([save = std::move(save)](bool value) {
		save(value);
	}, button->lifetime());
}

// `Settings` names Lumina::Settings inside this namespace, so the settings
// section helpers have to be reached through the global namespace.
void AddValueRow(
		not_null<Ui::VerticalLayout*> container,
		rpl::producer<QString> label,
		Fn<QString()> value,
		Fn<void()> activate) {
	::Settings::AddButtonWithLabel(
		container,
		std::move(label),
		LabelValue(std::move(value)),
		st::settingsButtonNoIcon
	)->setClickedCallback(std::move(activate));
}

void EditCodeBox(not_null<Ui::GenericBox*> box) {
	box->setTitle(TrValue(u"LuminaSecurityFakeCrashCodeDialogTitle"_q));

	// Shown in the clear, deliberately, and unlike Android - which masks the
	// field. The row behind this box only says "Set", so without this there is
	// no way to find out what the code actually is, and a duress code the user
	// is no longer sure of is a duress code they will not use. It is the same
	// trade the translation page already makes for API keys: masked in the
	// row, legible in the editor. Anyone who can read this box is looking at
	// an unlocked LuminaGram anyway.
	const auto field = box->addRow(object_ptr<Ui::InputField>(
		box,
		st::defaultInputField,
		Ui::InputField::Mode::SingleLine,
		TrValue(u"LuminaSecurityFakeCrashCodeHint"_q),
		FakeCrashCode()));
	field->setMaxLength(kCodeMaxLength);
	box->setFocusCallback([=] {
		field->setFocusFast();
	});

	// The passcode comparison below derives on a worker (see
	// CheckCodeIsLocalPasscode), so a second Save while it runs is ignored
	// rather than starting a second derivation.
	const auto checking = box->lifetime().make_state<bool>(false);
	const auto submit = [=] {
		if (*checking) {
			return;
		}
		const auto entered = field->getLastText().trimmed();

		// The duress code must not be the passcode. The lock screen would
		// still behave sanely if it were - it only consults the duress code
		// after the passcode has already been rejected, so a collision costs
		// the feature, not the account - but a code that silently never fires
		// is worth refusing at the one moment we can explain why.
		*checking = true;
		CheckCodeIsLocalPasscode(entered, box, [=](bool clash) {
			*checking = false;
			if (clash) {
				box->uiShow()->showBox(Ui::MakeInformBox(
					Tr(u"LuminaSecurityFakeCrashCodeSameAsPasscode"_q)));
				return;
			}
			SetFakeCrashCode(entered);
			box->closeBox();
		});
	};
	field->submits() | rpl::on_next(submit, field->lifetime());

	box->addButton(tr::lng_settings_save(), submit);
	box->addButton(tr::lng_cancel(), [=] {
		box->closeBox();
	});
}

// "When this code is entered": the two modes, as radio rows inside the block
// that only shows while the feature is on, like Android's. Placed under the
// code row rather than above it, so the code row is not read as a third
// choice; Exit is listed first because it is this platform's default.
void AddModeRows(not_null<Ui::VerticalLayout*> container) {
	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(
		container,
		TrValue(u"LuminaFakeCrashModeHeader"_q));
	Ui::AddSkip(container, st::settingsSendTypeSkip);

	const auto group = std::make_shared<Ui::RadioenumGroup<FakeCrashMode>>(
		CurrentFakeCrashMode());
	const auto add = [&](FakeCrashMode mode, const QString &key) {
		const auto row = container->add(
			object_ptr<Ui::Radioenum<FakeCrashMode>>(
				container,
				group,
				mode,
				Tr(key),
				st::settingsSendType),
			st::settingsSendTypePadding);
		LangChanges(
		) | rpl::on_next([=] {
			row->setText(Tr(key));
		}, row->lifetime());
	};
	add(FakeCrashMode::Exit, u"LuminaFakeCrashModeExit"_q);
	add(FakeCrashMode::Dialog, u"LuminaFakeCrashModeDialog"_q);

	// The preference can also move under the page - a restored backup writes
	// every key it carries - so the rows follow the preference rather than
	// only the clicks; `syncing` keeps that path from writing it straight
	// back (same shape as lumina_dialogs_style_settings.cpp).
	const auto syncing = container->lifetime().make_state<bool>(false);
	group->setChangedCallback([=](FakeCrashMode mode) {
		if (*syncing) {
			return;
		}
		SetFakeCrashMode(mode);
	});
	FakeCrashChanges(
	) | rpl::on_next([=] {
		*syncing = true;
		group->setValue(CurrentFakeCrashMode());
		*syncing = false;
	}, container->lifetime());

	Ui::AddSkip(container, st::settingsSendTypeSkip);
}

// One reactive paragraph rather than a stack of conditional dividers: the
// warnings come and go as the code, the toggle and the passcode change, and a
// block of dividers appearing and disappearing under the rows reads far worse
// than a paragraph that grows a sentence.
[[nodiscard]] QString AboutText(bool codeIsPasscode) {
	auto lines = QStringList();
	lines.append(Tr(u"LuminaSecurityFakeCrashInfo"_q));
	if (FakeCrashEnabled()) {
		// What the chosen mode looks like, the way Android puts it under
		// each of its two choices.
		lines.append(Tr((CurrentFakeCrashMode() == FakeCrashMode::Dialog)
			? u"LuminaFakeCrashModeDialogInfo"_q
			: u"LuminaFakeCrashModeExitInfo"_q));
	}

	// The decision recorded in the port plan (§6 Q3): a fingerprint opens the
	// app without the passcode field ever being touched, and would therefore
	// walk straight past the duress code. Said here because the toggle that
	// causes it is here, and the system-unlock switch that stops working is
	// on a different page entirely.
	lines.append(Tr(u"LuminaSecurityFakeCrashBiometricInfo"_q));

	if (FakeCrashEnabled() && FakeCrashCode().isEmpty()) {
		lines.append(Tr(u"LuminaSecurityFakeCrashNoCodeInfo"_q));
	}
	if (FakeCrashEnabled() && !HasLocalPasscode()) {
		lines.append(Tr(u"LuminaSecurityFakeCrashNoPasscodeInfo"_q));
	}
	if (FakeCrashArmed() && codeIsPasscode) {
		// Only reachable by changing the passcode lock to the duress code
		// after setting it, which the passcode page knows nothing about.
		lines.append(Tr(u"LuminaSecurityFakeCrashCodeClashInfo"_q));
	}
	return lines.join(u" "_q);
}

} // namespace

void AddFakeCrashRows(
		not_null<Ui::VerticalLayout*> container,
		not_null<Window::SessionController*> controller) {
	Ui::AddSkip(container);
	Ui::AddSubsectionTitle(
		container,
		TrValue(u"LuminaSecurityFakeCrashHeader"_q));
	AddToggleRow(
		container,
		TrValue(u"LuminaSecurityFakeCrashEnable"_q),
		[] { return FakeCrashEnabled(); },
		[](bool value) { SetFakeCrashEnabled(value); });

	// Built once and toggled reactively, like the provider blocks on the
	// translation page: switching the toggle never rebuilds the block and
	// never invalidates a pointer another row is holding.
	const auto codeBlock = container->add(
		object_ptr<Ui::SlideWrap<Ui::VerticalLayout>>(
			container,
			object_ptr<Ui::VerticalLayout>(container))
	)->toggleOn(
		FlagValue([] { return FakeCrashEnabled(); })
	)->finishAnimating()->entity();
	AddValueRow(
		codeBlock,
		TrValue(u"LuminaSecurityFakeCrashCode"_q),
		[] {
			return Tr(FakeCrashCode().isEmpty()
				? u"LuminaSecurityFakeCrashCodeNotSet"_q
				: u"LuminaSecurityFakeCrashCodeSet"_q);
		},
		[=] { controller->show(Box(EditCodeBox)); });
	AddModeRows(codeBlock);

	// The clash warning needs a passcode derivation, which since 7.3 runs on
	// a worker (see CheckCodeIsLocalPasscode): the paragraph shows the last
	// verdict and is rebuilt when a new one arrives. Every change that can
	// move it starts a new check; a generation counter drops a verdict that
	// arrives for a state that has changed since.
	const auto codeIsPasscode = container->lifetime().make_state<
		rpl::variable<bool>>(false);
	const auto generation = container->lifetime().make_state<int>(0);
	rpl::single(
		rpl::empty
	) | rpl::then(
		Changes()
	) | rpl::on_next([=] {
		const auto now = ++*generation;
		if (!FakeCrashArmed()) {
			*codeIsPasscode = false;
			return;
		}
		CheckCodeIsLocalPasscode(FakeCrashCode(), container, [=](bool clash) {
			if (*generation == now) {
				*codeIsPasscode = clash;
			}
		});
	}, container->lifetime());

	Ui::AddSkip(container);
	Ui::AddDividerText(
		container,
		rpl::combine(
			rpl::single(rpl::empty) | rpl::then(Changes()),
			codeIsPasscode->value()
		) | rpl::map([](rpl::empty_value, bool clash) {
			return AboutText(clash);
		}));
}

} // namespace Lumina
