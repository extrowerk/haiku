/*
 * Copyright 2013-2014, Stephan Aßmus <superstippi@gmx.de>.
 * All rights reserved. Distributed under the terms of the MIT License.
 */

#include "PackageInfoView.h"

#include <algorithm>
#include <stdio.h>

#include <Alert.h>
#include <Autolock.h>
#include <Bitmap.h>
#include <Button.h>
#include <CardLayout.h>
#include <Catalog.h>
#include <Font.h>
#include <GridView.h>
#include <LayoutBuilder.h>
#include <LayoutUtils.h>
#include <LocaleRoster.h>
#include <Message.h>
#include <OutlineListView.h>
#include <ScrollView.h>
#include <SpaceLayoutItem.h>
#include <StatusBar.h>
#include <StringView.h>
#include <TabView.h>
#include <Url.h>

#include <package/hpkg/PackageReader.h>
#include <package/hpkg/NoErrorOutput.h>
#include <package/hpkg/PackageContentHandler.h>
#include <package/hpkg/PackageEntry.h>

#include "BitmapButton.h"
#include "BitmapView.h"
#include "LinkView.h"
#include "LinkedBitmapView.h"
#include "MarkupTextView.h"
#include "MessagePackageListener.h"
#include "PackageActionHandler.h"
#include "PackageContentsView.h"
#include "PackageInfo.h"
#include "PackageManager.h"
#include "RatingView.h"
#include "ScrollableGroupView.h"
#include "TextView.h"


#undef B_TRANSLATION_CONTEXT
#define B_TRANSLATION_CONTEXT "PackageInfoView"


static const rgb_color kLightBlack = (rgb_color) { 60, 60, 60, 255 };
static const float kContentTint = (B_NO_TINT + B_LIGHTEN_1_TINT) / 2.0f;


//! Layouts the scrollbar so it looks nice with no border and the document
// window look.
class CustomScrollView : public BScrollView {
public:
	CustomScrollView(const char* name, BView* target)
		:
		BScrollView(name, target, 0, false, true, B_NO_BORDER)
	{
	}

	virtual void DoLayout()
	{
		BRect innerFrame = Bounds();
		innerFrame.right -= B_V_SCROLL_BAR_WIDTH + 1;

		BView* target = Target();
		if (target != NULL) {
			Target()->MoveTo(innerFrame.left, innerFrame.top);
			Target()->ResizeTo(innerFrame.Width(), innerFrame.Height());
		}

		BScrollBar* scrollBar = ScrollBar(B_VERTICAL);
		if (scrollBar != NULL) {
			BRect rect = innerFrame;
			rect.left = rect.right + 1;
			rect.right = rect.left + B_V_SCROLL_BAR_WIDTH;
			rect.bottom -= B_H_SCROLL_BAR_HEIGHT;

			scrollBar->MoveTo(rect.left, rect.top);
			scrollBar->ResizeTo(rect.Width(), rect.Height());
		}
	}
};


class RatingsScrollView : public CustomScrollView {
public:
	RatingsScrollView(const char* name, BView* target)
		:
		CustomScrollView(name, target)
	{
	}

	virtual void DoLayout()
	{
		CustomScrollView::DoLayout();

		BScrollBar* scrollBar = ScrollBar(B_VERTICAL);
		BView* target = Target();
		if (target != NULL && scrollBar != NULL) {
			// Set the scroll steps
			BView* item = target->ChildAt(0);
			if (item != NULL) {
				scrollBar->SetSteps(item->MinSize().height + 1,
					item->MinSize().height + 1);
			}
		}
	}
};


// #pragma mark - rating stats


class DiagramBarView : public BView {
public:
	DiagramBarView()
		:
		BView("diagram bar view", B_WILL_DRAW),
		fValue(0.0f)
	{
		SetViewColor(B_TRANSPARENT_COLOR);
		SetLowColor(ui_color(B_PANEL_BACKGROUND_COLOR));
		SetHighColor(tint_color(LowColor(), B_DARKEN_2_TINT));
	}

	virtual ~DiagramBarView()
	{
	}

	virtual void AttachedToWindow()
	{
		BView* parent = Parent();
		if (parent != NULL) {
			SetLowColor(parent->ViewColor());
			SetHighColor(tint_color(LowColor(), B_DARKEN_2_TINT));
		}
	}

	virtual void Draw(BRect updateRect)
	{
		FillRect(updateRect, B_SOLID_LOW);

		if (fValue <= 0.0f)
			return;

		BRect rect(Bounds());
		rect.right = ceilf(rect.left + fValue * rect.Width());

		FillRect(rect, B_SOLID_HIGH);
	}

	virtual BSize MinSize()
	{
		return BSize(64, 10);
	}

	virtual BSize PreferredSize()
	{
		return MinSize();
	}

	virtual BSize MaxSize()
	{
		return BSize(64, 10);
	}

	void SetValue(float value)
	{
		if (fValue != value) {
			fValue = value;
			Invalidate();
		}
	}

private:
	float			fValue;
};


// #pragma mark - TitleView


enum {
	MSG_PACKAGE_ACTION			= 'pkga',
	MSG_MOUSE_ENTERED_RATING	= 'menr',
	MSG_MOUSE_EXITED_RATING		= 'mexr',
};


class TransitReportingButton : public BButton {
public:
	TransitReportingButton(const char* name, const char* label,
			BMessage* message)
		:
		BButton(name, label, message),
		fTransitMessage(NULL)
	{
	}

	virtual ~TransitReportingButton()
	{
		SetTransitMessage(NULL);
	}

	virtual void MouseMoved(BPoint point, uint32 transit,
		const BMessage* dragMessage)
	{
		BButton::MouseMoved(point, transit, dragMessage);

		if (fTransitMessage != NULL && transit == B_EXITED_VIEW)
			Invoke(fTransitMessage);
	}

	void SetTransitMessage(BMessage* message)
	{
		if (fTransitMessage != message) {
			delete fTransitMessage;
			fTransitMessage = message;
		}
	}

private:
	BMessage*	fTransitMessage;
};


class TransitReportingRatingView : public RatingView, public BInvoker {
public:
	TransitReportingRatingView(BMessage* transitMessage)
		:
		RatingView("package rating view"),
		fTransitMessage(transitMessage)
	{
	}

	virtual ~TransitReportingRatingView()
	{
		delete fTransitMessage;
	}

	virtual void MouseMoved(BPoint point, uint32 transit,
		const BMessage* dragMessage)
	{
		RatingView::MouseMoved(point, transit, dragMessage);

		if (fTransitMessage != NULL && transit == B_ENTERED_VIEW)
			Invoke(fTransitMessage);
	}

private:
	BMessage*	fTransitMessage;
};


class TitleView : public BGroupView {
public:
	TitleView()
		:
		BGroupView("title view", B_HORIZONTAL)
	{
		fIconView = new BitmapView("package icon view");
		fTitleView = new BStringView("package title view", "");
		fPublisherView = new BStringView("package publisher view", "");

		// Title font
		BFont font;
		GetFont(&font);
		font_family family;
		font_style style;
		font.SetSize(ceilf(font.Size() * 1.5f));
		font.GetFamilyAndStyle(&family, &style);
		font.SetFamilyAndStyle(family, "Bold");
		fTitleView->SetFont(&font);

		// Publisher font
		GetFont(&font);
		font.SetSize(std::max(9.0f, floorf(font.Size() * 0.92f)));
		font.SetFamilyAndStyle(family, "Italic");
		fPublisherView->SetFont(&font);
		fPublisherView->SetHighColor(kLightBlack);

		// slightly bigger font
		GetFont(&font);
		font.SetSize(ceilf(font.Size() * 1.2f));

		// Version info
		fVersionInfo = new BStringView("package version info", "");
		fVersionInfo->SetFont(&font);
		fVersionInfo->SetHighColor(kLightBlack);

		// Rating view
		fRatingView = new TransitReportingRatingView(
			new BMessage(MSG_MOUSE_ENTERED_RATING));

		fAvgRating = new BStringView("package average rating", "");
		fAvgRating->SetFont(&font);
		fAvgRating->SetHighColor(kLightBlack);

		fVoteInfo = new BStringView("package vote info", "");
		// small font
		GetFont(&font);
		font.SetSize(std::max(9.0f, floorf(font.Size() * 0.85f)));
		fVoteInfo->SetFont(&font);
		fVoteInfo->SetHighColor(kLightBlack);

		// Rate button
		fRateButton = new TransitReportingButton("rate",
			B_TRANSLATE("Rate package" B_UTF8_ELLIPSIS),
			new BMessage(MSG_RATE_PACKAGE));
		fRateButton->SetTransitMessage(new BMessage(MSG_MOUSE_EXITED_RATING));
		fRateButton->SetExplicitAlignment(BAlignment(B_ALIGN_LEFT,
			B_ALIGN_VERTICAL_CENTER));

		// Rating group
		BView* ratingStack = new BView("rating stack", 0);
		fRatingLayout = new BCardLayout();
		ratingStack->SetLayout(fRatingLayout);
		ratingStack->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
		ratingStack->SetViewColor(ui_color(B_PANEL_BACKGROUND_COLOR));

		BGroupView* ratingGroup = new BGroupView(B_HORIZONTAL,
			B_USE_SMALL_SPACING);
		BLayoutBuilder::Group<>(ratingGroup)
			.Add(fRatingView)
			.Add(fAvgRating)
			.Add(fVoteInfo)
		;

		ratingStack->AddChild(ratingGroup);
		ratingStack->AddChild(fRateButton);
		fRatingLayout->SetVisibleItem((int32)0);

		BLayoutBuilder::Group<>(this)
			.Add(fIconView)
			.AddGroup(B_VERTICAL, 1.0f, 2.2f)
				.Add(fTitleView)
				.Add(fPublisherView)
				.SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET))
			.End()
			.AddGlue(0.1f)
			.Add(ratingStack, 0.8f)
			.AddGlue(0.2f)
			.AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING, 2.0f)
				.Add(fVersionInfo)
				.AddGlue()
				.SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET))
			.End()
		;

		Clear();
	}

	virtual ~TitleView()
	{
	}

	virtual void AttachedToWindow()
	{
		fRateButton->SetTarget(this);
		fRatingView->SetTarget(this);
	}

	virtual void MessageReceived(BMessage* message)
	{
		switch (message->what) {
			case MSG_RATE_PACKAGE:
				// Forward to window (The button has us as target so
				// we receive the message below.)
				Window()->PostMessage(MSG_RATE_PACKAGE);
				break;

			case MSG_MOUSE_ENTERED_RATING:
				fRatingLayout->SetVisibleItem(1);
				break;

			case MSG_MOUSE_EXITED_RATING:
				fRatingLayout->SetVisibleItem((int32)0);
				break;
		}
	}

	void SetPackage(const PackageInfo& package)
	{
		if (package.Icon().Get() != NULL)
			fIconView->SetBitmap(package.Icon(), SharedBitmap::SIZE_32);
		else
			fIconView->UnsetBitmap();

		fTitleView->SetText(package.Title());

		BString publisher = package.Publisher().Name();
		fPublisherView->SetText(publisher);

		BString version = B_TRANSLATE("%Version%");
		version.ReplaceAll("%Version%", package.Version().ToString());
		fVersionInfo->SetText(version);

		RatingSummary ratingSummary = package.CalculateRatingSummary();

		fRatingView->SetRating(ratingSummary.averageRating);

		if (ratingSummary.ratingCount > 0) {
			BString avgRating;
			avgRating.SetToFormat("%.1f", ratingSummary.averageRating);
			fAvgRating->SetText(avgRating);

			BString votes;
			votes.SetToFormat("%d", ratingSummary.ratingCount);

			BString voteInfo(B_TRANSLATE("(%Votes%)"));
			voteInfo.ReplaceAll("%Votes%", votes);

			fVoteInfo->SetText(voteInfo);
		} else {
			fAvgRating->SetText("");
			fVoteInfo->SetText(B_TRANSLATE("n/a"));
		}

		InvalidateLayout();
		Invalidate();
	}

	void Clear()
	{
		fIconView->UnsetBitmap();
		fTitleView->SetText("");
		fPublisherView->SetText("");
		fVersionInfo->SetText("");
		fRatingView->SetRating(-1.0f);
		fAvgRating->SetText("");
		fVoteInfo->SetText("");
	}

private:
	BitmapView*						fIconView;

	BStringView*					fTitleView;
	BStringView*					fPublisherView;

	BStringView*					fVersionInfo;

	BCardLayout*					fRatingLayout;

	TransitReportingRatingView*		fRatingView;
	BStringView*					fAvgRating;
	BStringView*					fVoteInfo;

	TransitReportingButton*			fRateButton;
};


// #pragma mark - PackageActionView


class PackageActionView : public BView {
public:
	PackageActionView(PackageActionHandler* handler)
		:
		BView("about view", B_WILL_DRAW),
		fLayout(new BGroupLayout(B_HORIZONTAL)),
		fPackageActionHandler(handler),
		fStatusLabel(NULL),
		fStatusBar(NULL)
	{
		SetViewColor(ui_color(B_PANEL_BACKGROUND_COLOR));

		SetLayout(fLayout);
		fLayout->AddItem(BSpaceLayoutItem::CreateGlue());
	}

	virtual ~PackageActionView()
	{
		Clear();
	}

	virtual void MessageReceived(BMessage* message)
	{
		switch (message->what) {
			case MSG_PACKAGE_ACTION:
				_RunPackageAction(message);
				break;

			default:
				BView::MessageReceived(message);
				break;
		}
	}

	void SetPackage(const PackageInfo& package)
	{
		if (package.State() == DOWNLOADING) {
			AdoptDownloadProgress(package);
		} else {
			AdoptActions(package);
		}

	}

	void AdoptActions(const PackageInfo& package)
	{
		PackageManager manager(
			BPackageKit::B_PACKAGE_INSTALLATION_LOCATION_HOME);

		// TODO: if the given package is either a system package
		// or a system dependency, show a message indicating that status
		// so the user knows why no actions are presented
		PackageActionList actions = manager.GetPackageActions(
			const_cast<PackageInfo*>(&package),
			fPackageActionHandler->GetModel());

		bool clearNeeded = fStatusBar != NULL;
		if (!clearNeeded) {
			if (actions.CountItems() != fPackageActions.CountItems())
				clearNeeded = true;
			else {
				for (int32 i = 0; i < actions.CountItems(); i++) {
					if (actions.ItemAtFast(i)->Type()
							!= fPackageActions.ItemAtFast(i)->Type()) {
						clearNeeded = true;
						break;
					}
				}
			}
		}

		fPackageActions = actions;
		if (!clearNeeded && fButtons.CountItems() == actions.CountItems()) {
			int32 index = 0;
			for (int32 i = fPackageActions.CountItems() - 1; i >= 0; i--) {
				const PackageActionRef& action = fPackageActions.ItemAtFast(i);
				BButton* button = (BButton*)fButtons.ItemAtFast(index++);
				button->SetLabel(action->Label());
			}
			return;
		}

		Clear();

		// Add Buttons in reverse action order
		for (int32 i = fPackageActions.CountItems() - 1; i >= 0; i--) {
			const PackageActionRef& action = fPackageActions.ItemAtFast(i);

			BMessage* message = new BMessage(MSG_PACKAGE_ACTION);
			message->AddInt32("index", i);

			BButton* button = new BButton(action->Label(), message);
			fLayout->AddView(button);
			button->SetTarget(this);

			fButtons.AddItem(button);
		}
	}

	void AdoptDownloadProgress(const PackageInfo& package)
	{
		if (fButtons.CountItems() > 0)
			Clear();

		if (fStatusBar == NULL) {
			fStatusLabel = new BStringView("progress label",
				B_TRANSLATE("Downloading:"));
			fLayout->AddView(fStatusLabel);

			fStatusBar = new BStatusBar("progress");
			fStatusBar->SetMaxValue(100.0);
			fStatusBar->SetExplicitMinSize(
				BSize(StringWidth("XXX") * 5, B_SIZE_UNSET));

			fLayout->AddView(fStatusBar);
		}

		fStatusBar->SetTo(package.DownloadProgress() * 100.0);
	}

	void Clear()
	{
		for (int32 i = fButtons.CountItems() - 1; i >= 0; i--) {
			BButton* button = (BButton*)fButtons.ItemAtFast(i);
			button->RemoveSelf();
			delete button;
		}
		fButtons.MakeEmpty();

		if (fStatusBar != NULL) {
			fStatusBar->RemoveSelf();
			delete fStatusBar;
			fStatusBar = NULL;
		}
		if (fStatusLabel != NULL) {
			fStatusLabel->RemoveSelf();
			delete fStatusLabel;
			fStatusLabel = NULL;
		}
	}

private:
	void _RunPackageAction(BMessage* message)
	{
		int32 index;
		if (message->FindInt32("index", &index) != B_OK)
			return;

		const PackageActionRef& action = fPackageActions.ItemAt(index);
		if (action.Get() == NULL)
			return;

		PackageActionList actions;
		actions.Add(action);
		status_t result
			= fPackageActionHandler->SchedulePackageActions(actions);

		if (result != B_OK) {
			fprintf(stderr, "Failed to schedule action: "
				"%s '%s': %s\n", action->Label(),
				action->Package()->Name().String(),
				strerror(result));
			BString message(B_TRANSLATE("The package action "
				"could not be scheduled: %Error%"));
			message.ReplaceAll("%Error%", strerror(result));
			BAlert* alert = new(std::nothrow) BAlert(
				B_TRANSLATE("Package action failed"),
				message, B_TRANSLATE("OK"), NULL, NULL,
				B_WIDTH_AS_USUAL, B_WARNING_ALERT);
			if (alert != NULL)
				alert->Go();
		} else {
			// Find the button for this action and disable it.
			// Actually search the matching button instead of just using
			// fButtons.ItemAt((fButtons.CountItems() - 1) - index) to
			// make this robust against for example changing the order of
			// buttons from right -> left to left -> right...
			for (int32 i = 0; i < fButtons.CountItems(); i++) {
				BButton* button = (BButton*)fButtons.ItemAt(index);
				if (button == NULL)
					continue;
				BMessage* buttonMessage = button->Message();
				if (buttonMessage == NULL)
					continue;
				int32 buttonIndex;
				if (buttonMessage->FindInt32("index", &buttonIndex) != B_OK)
					continue;
				if (buttonIndex == index) {
					button->SetEnabled(false);
					break;
				}
			}
		}
	}

private:
	BGroupLayout*		fLayout;
	PackageActionList	fPackageActions;
	PackageActionHandler* fPackageActionHandler;
	BList				fButtons;

	BStringView*		fStatusLabel;
	BStatusBar*			fStatusBar;
};


// #pragma mark - AboutView


enum {
	MSG_EMAIL_PUBLISHER				= 'emlp',
	MSG_VISIT_PUBLISHER_WEBSITE		= 'vpws',
};


class AboutView : public BView {
public:
	AboutView()
		:
		BView("about view", 0),
		fEmailIcon("text/x-email"),
		fWebsiteIcon("text/html")
	{
		SetViewColor(tint_color(ui_color(B_PANEL_BACKGROUND_COLOR),
			kContentTint));

		fDescriptionView = new MarkupTextView("description view");
		fDescriptionView->SetLowColor(ViewColor());
		fDescriptionView->SetInsets(be_plain_font->Size());

		BScrollView* scrollView = new CustomScrollView(
			"description scroll view", fDescriptionView);

		BFont smallFont;
		GetFont(&smallFont);
		smallFont.SetSize(std::max(9.0f, ceilf(smallFont.Size() * 0.85f)));

		// TODO: Clicking the screen shot view should open ShowImage with the
		// the screen shot. This could be done by writing the screen shot to
		// a temporary folder, launching ShowImage to display it, and writing
		// all other screenshots associated with the package to the same folder
		// so the user can use the ShowImage navigation to view the other
		// screenshots.
		fScreenshotView = new LinkedBitmapView("screenshot view",
			new BMessage(MSG_SHOW_SCREENSHOT));
		fScreenshotView->SetExplicitMinSize(BSize(64.0f, 64.0f));
		fScreenshotView->SetExplicitMaxSize(
			BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));
		fScreenshotView->SetExplicitAlignment(
			BAlignment(B_ALIGN_CENTER, B_ALIGN_TOP));

		fEmailIconView = new BitmapView("email icon view");
		fEmailLinkView = new LinkView("email link view", "",
			new BMessage(MSG_EMAIL_PUBLISHER), kLightBlack);
		fEmailLinkView->SetFont(&smallFont);

		fWebsiteIconView = new BitmapView("website icon view");
		fWebsiteLinkView = new LinkView("website link view", "",
			new BMessage(MSG_VISIT_PUBLISHER_WEBSITE), kLightBlack);
		fWebsiteLinkView->SetFont(&smallFont);

		BGroupView* leftGroup = new BGroupView(B_VERTICAL,
			B_USE_DEFAULT_SPACING);
		leftGroup->SetViewColor(ViewColor());

		BLayoutBuilder::Group<>(this, B_HORIZONTAL, 0.0f)
//			.Add(BSpaceLayoutItem::CreateHorizontalStrut(32.0f))
			.AddGroup(leftGroup, 1.0f)
				.Add(fScreenshotView)
				.AddGroup(B_HORIZONTAL)
					.AddGrid(B_USE_HALF_ITEM_SPACING, B_USE_HALF_ITEM_SPACING)
						.Add(fEmailIconView, 0, 0)
						.Add(fEmailLinkView, 1, 0)
						.Add(fWebsiteIconView, 0, 1)
						.Add(fWebsiteLinkView, 1, 1)
					.End()
				.End()
				.SetInsets(B_USE_DEFAULT_SPACING)
				.SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET))
			.End()
			.Add(scrollView, 2.0f)

			.SetExplicitMaxSize(BSize(B_SIZE_UNSET, B_SIZE_UNLIMITED))
			.SetInsets(0.0f, -1.0f, -1.0f, -1.0f)
		;
	}

	virtual ~AboutView()
	{
		Clear();
	}

	virtual void AttachedToWindow()
	{
		fScreenshotView->SetTarget(this);
		fEmailLinkView->SetTarget(this);
		fWebsiteLinkView->SetTarget(this);
	}

	virtual void MessageReceived(BMessage* message)
	{
		switch (message->what) {
			case MSG_SHOW_SCREENSHOT:
			{
				// Forward to window for now
				Window()->PostMessage(message, Window());
				break;
			}

			case MSG_EMAIL_PUBLISHER:
			{
				// TODO: Implement. If memory serves, there is a
				// standard command line interface which mail apps should
				// support, i.e. to open a compose window with the TO: field
				// already set.
				break;
			}

			case MSG_VISIT_PUBLISHER_WEBSITE:
			{
				BUrl url(fWebsiteLinkView->Text());
				url.OpenWithPreferredApplication();
				break;
			}

			default:
				BView::MessageReceived(message);
				break;
		}
	}

	void SetPackage(const PackageInfo& package)
	{
		fDescriptionView->SetText(package.ShortDescription(),
			package.FullDescription());

		fEmailIconView->SetBitmap(&fEmailIcon, SharedBitmap::SIZE_16);
		_SetContactInfo(fEmailLinkView, package.Publisher().Email());
		fWebsiteIconView->SetBitmap(&fWebsiteIcon, SharedBitmap::SIZE_16);
		_SetContactInfo(fWebsiteLinkView, package.Publisher().Website());

		bool hasScreenshot = false;
		const BitmapList& screenShots = package.Screenshots();
		if (screenShots.CountItems() > 0) {
			const BitmapRef& bitmapRef = screenShots.ItemAtFast(0);
			if (bitmapRef.Get() != NULL) {
				hasScreenshot = true;
				fScreenshotView->SetBitmap(bitmapRef);
			}
		}

		if (!hasScreenshot)
			fScreenshotView->UnsetBitmap();

		fScreenshotView->SetEnabled(hasScreenshot);
	}

	void Clear()
	{
		fDescriptionView->SetText("");
		fEmailIconView->UnsetBitmap();
		fEmailLinkView->SetText("");
		fWebsiteIconView->UnsetBitmap();
		fWebsiteLinkView->SetText("");

		fScreenshotView->UnsetBitmap();
		fScreenshotView->SetEnabled(false);
	}

private:
	void _SetContactInfo(LinkView* view, const BString& string)
	{
		if (string.Length() > 0) {
			view->SetText(string);
			view->SetEnabled(true);
		} else {
			view->SetText(B_TRANSLATE("<no info>"));
			view->SetEnabled(false);
		}
	}

private:
	MarkupTextView*		fDescriptionView;

	LinkedBitmapView*	fScreenshotView;

	SharedBitmap		fEmailIcon;
	BitmapView*			fEmailIconView;
	LinkView*			fEmailLinkView;

	SharedBitmap		fWebsiteIcon;
	BitmapView*			fWebsiteIconView;
	LinkView*			fWebsiteLinkView;
};


// #pragma mark - UserRatingsView


class RatingItemView : public BGroupView {
public:
	RatingItemView(const UserRating& rating, const BitmapRef& voteUpIcon,
		const BitmapRef& voteDownIcon)
		:
		BGroupView(B_HORIZONTAL, 0.0f)
	{
		SetViewColor(tint_color(ui_color(B_PANEL_BACKGROUND_COLOR),
			kContentTint));

		fAvatarView = new BitmapView("avatar view");
		if (rating.User().Avatar().Get() != NULL) {
			fAvatarView->SetBitmap(rating.User().Avatar(),
				SharedBitmap::SIZE_16);
		}
		fAvatarView->SetExplicitMinSize(BSize(16.0f, 16.0f));

		fNameView = new BStringView("user name", rating.User().NickName());
		BFont nameFont(be_bold_font);
		nameFont.SetSize(std::max(9.0f, floorf(nameFont.Size() * 0.9f)));
		fNameView->SetFont(&nameFont);
		fNameView->SetHighColor(kLightBlack);
		fNameView->SetExplicitMaxSize(
			BSize(nameFont.StringWidth("xxxxxxxxxxxxxxxxxxxxxx"), B_SIZE_UNSET));

		fRatingView = new RatingView("package rating view");
		fRatingView->SetRating(rating.Rating());

		BString ratingLabel;
		if (rating.Rating() >= 0.0f)
			ratingLabel.SetToFormat("%.1f", rating.Rating());
		fRatingLabelView = new BStringView("rating label", ratingLabel);

		BString versionLabel(B_TRANSLATE("for %Version%"));
		versionLabel.ReplaceAll("%Version%", rating.PackageVersion());
		fPackageVersionView = new BStringView("package version",
			versionLabel);
		BFont versionFont(be_plain_font);
		versionFont.SetSize(std::max(9.0f, floorf(versionFont.Size() * 0.85f)));
		fPackageVersionView->SetFont(&versionFont);
		fPackageVersionView->SetHighColor(kLightBlack);

		// TODO: User rating IDs to identify which rating to vote up or down
//		BMessage* voteUpMessage = new BMessage(MSG_VOTE_UP);
//		voteUpMessage->AddInt32("rating id", -1);
//		BMessage* voteDownMessage = new BMessage(MSG_VOTE_DOWN);
//		voteDownMessage->AddInt32("rating id", -1);
//
//		fVoteUpIconView = new BitmapButton("vote up icon", voteUpMessage);
//		fUpVoteCountView = new BStringView("up vote count", "");
//		fVoteDownIconView = new BitmapButton("vote down icon", voteDownMessage);
//		fDownVoteCountView = new BStringView("up vote count", "");
//
//		fVoteUpIconView->SetBitmap(voteUpIcon, SharedBitmap::SIZE_16);
//		fVoteDownIconView->SetBitmap(voteDownIcon, SharedBitmap::SIZE_16);
//
//		fUpVoteCountView->SetFont(&versionFont);
//		fUpVoteCountView->SetHighColor(kLightBlack);
//		fDownVoteCountView->SetFont(&versionFont);
//		fDownVoteCountView->SetHighColor(kLightBlack);
//
//		BString voteCountLabel;
//		voteCountLabel.SetToFormat("%" B_PRId32, rating.UpVotes());
//		fUpVoteCountView->SetText(voteCountLabel);
//		voteCountLabel.SetToFormat("%" B_PRId32, rating.DownVotes());
//		fDownVoteCountView->SetText(voteCountLabel);

		fTextView = new TextView("rating text");
		fTextView->SetViewColor(ViewColor());
		ParagraphStyle paragraphStyle(fTextView->ParagraphStyle());
		paragraphStyle.SetJustify(true);
		fTextView->SetParagraphStyle(paragraphStyle);

		fTextView->SetText(rating.Comment());

		BLayoutBuilder::Group<>(this)
			.Add(fAvatarView, 0.2f)
			.AddGroup(B_VERTICAL, 0.0f)
				.AddGroup(B_HORIZONTAL, B_USE_DEFAULT_SPACING)
					.Add(fNameView)
					.Add(fRatingView)
					.Add(fRatingLabelView)
					.AddGlue(0.1f)
					.Add(fPackageVersionView)
					.AddGlue(5.0f)
//					.AddGroup(B_HORIZONTAL, 0.0f, 0.0f)
//						.Add(fVoteUpIconView)
//						.Add(fUpVoteCountView)
//						.AddStrut(B_USE_HALF_ITEM_SPACING)
//						.Add(fVoteDownIconView)
//						.Add(fDownVoteCountView)
//					.End()
				.End()
				.Add(fTextView)
			.End()
			.SetInsets(B_USE_DEFAULT_SPACING)
		;
	}

private:
	BitmapView*		fAvatarView;
	BStringView*	fNameView;
	RatingView*		fRatingView;
	BStringView*	fRatingLabelView;
	BStringView*	fPackageVersionView;

//	BitmapView*		fVoteUpIconView;
//	BStringView*	fUpVoteCountView;
//	BitmapView*		fVoteDownIconView;
//	BStringView*	fDownVoteCountView;

	TextView*		fTextView;
};


class RatingSummaryView : public BGridView {
public:
	RatingSummaryView()
		:
		BGridView("rating summary view", B_USE_HALF_ITEM_SPACING, 0.0f)
	{
		SetViewColor(tint_color(ui_color(B_PANEL_BACKGROUND_COLOR),
			kContentTint - 0.1));

		BLayoutBuilder::Grid<> layoutBuilder(this);

		BFont smallFont;
		GetFont(&smallFont);
		smallFont.SetSize(std::max(9.0f, floorf(smallFont.Size() * 0.85f)));

		for (int32 i = 0; i < 5; i++) {
			BString label;
			label.SetToFormat("%" B_PRId32, 5 - i);
			fLabelViews[i] = new BStringView("", label);
			fLabelViews[i]->SetFont(&smallFont);
			fLabelViews[i]->SetHighColor(kLightBlack);
			layoutBuilder.Add(fLabelViews[i], 0, i);

			fDiagramBarViews[i] = new DiagramBarView();
			layoutBuilder.Add(fDiagramBarViews[i], 1, i);

			fCountViews[i] = new BStringView("", "");
			fCountViews[i]->SetFont(&smallFont);
			fCountViews[i]->SetHighColor(kLightBlack);
			fCountViews[i]->SetAlignment(B_ALIGN_RIGHT);
			layoutBuilder.Add(fCountViews[i], 2, i);
		}

		layoutBuilder.SetInsets(5);
	}

	void SetToSummary(const RatingSummary& summary) {
		for (int32 i = 0; i < 5; i++) {
			int32 count = summary.ratingCountByStar[4 - i];

			BString label;
			label.SetToFormat("%" B_PRId32, count);
			fCountViews[i]->SetText(label);

			if (summary.ratingCount > 0) {
				fDiagramBarViews[i]->SetValue(
					(float)count / summary.ratingCount);
			} else
				fDiagramBarViews[i]->SetValue(0.0f);
		}
	}

	void Clear() {
		for (int32 i = 0; i < 5; i++) {
			fCountViews[i]->SetText("");
			fDiagramBarViews[i]->SetValue(0.0f);
		}
	}

private:
	BStringView*	fLabelViews[5];
	DiagramBarView*	fDiagramBarViews[5];
	BStringView*	fCountViews[5];
};


class UserRatingsView : public BGroupView {
public:
	UserRatingsView()
		:
		BGroupView("package ratings view", B_HORIZONTAL),
		fThumbsUpIcon(BitmapRef(new SharedBitmap(502), true)),
		fThumbsDownIcon(BitmapRef(new SharedBitmap(503), true))
	{
		SetViewColor(tint_color(ui_color(B_PANEL_BACKGROUND_COLOR),
			kContentTint));

		fRatingSummaryView = new RatingSummaryView();

		ScrollableGroupView* ratingsContainerView = new ScrollableGroupView();
		ratingsContainerView->SetViewColor(
			tint_color(ui_color(B_PANEL_BACKGROUND_COLOR), kContentTint));
		fRatingContainerLayout = ratingsContainerView->GroupLayout();

		BScrollView* scrollView = new RatingsScrollView(
			"ratings scroll view", ratingsContainerView);

		BLayoutBuilder::Group<>(this)
			.AddGroup(B_VERTICAL)
				.Add(fRatingSummaryView, 0.0f)
				.AddGlue()
				.SetInsets(0.0f, B_USE_DEFAULT_SPACING, 0.0f, 0.0f)
			.End()
			.Add(scrollView, 1.0f)
			.SetInsets(B_USE_DEFAULT_SPACING, -1.0f, -1.0f, -1.0f)
		;

		_InitPreferredLanguages();
	}

	virtual ~UserRatingsView()
	{
		Clear();
	}

	void SetPackage(const PackageInfo& package)
	{
		ClearRatings();

		// TODO: Re-use rating summary already used for TitleView...
		fRatingSummaryView->SetToSummary(package.CalculateRatingSummary());

		const UserRatingList& userRatings = package.UserRatings();

		int count = userRatings.CountItems();
		if (count == 0) {
			BStringView* noRatingsView = new BStringView("no ratings",
				B_TRANSLATE("No user ratings available."));
			noRatingsView->SetAlignment(B_ALIGN_CENTER);
			noRatingsView->SetHighColor(kLightBlack);
			noRatingsView->SetExplicitMaxSize(
				BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));
			fRatingContainerLayout->AddView(0, noRatingsView);
			return;
		}

		// TODO: Sort by age or usefullness rating
		for (int i = count - 1; i >= 0; i--) {
			const UserRating& rating = userRatings.ItemAtFast(i);
			// Prevent ratings from showing that have a comment which
			// is in another language
			if (!rating.Comment().IsEmpty()
				&& fPreferredLanguages.CountItems() > 0
				&& !fPreferredLanguages.Contains(rating.Language())) {
				continue;
			}
			RatingItemView* view = new RatingItemView(rating, fThumbsUpIcon,
				fThumbsDownIcon);
			fRatingContainerLayout->AddView(0, view);
		}
	}

	void Clear()
	{
		fRatingSummaryView->Clear();
		ClearRatings();
	}

	void ClearRatings()
	{
		for (int32 i = fRatingContainerLayout->CountItems() - 1;
				BLayoutItem* item = fRatingContainerLayout->ItemAt(i); i--) {
			BView* view = dynamic_cast<RatingItemView*>(item->View());
			if (view == NULL)
				view = dynamic_cast<BStringView*>(item->View());
			if (view != NULL) {
				view->RemoveSelf();
				delete view;
			}
		}
	}

private:
	void _InitPreferredLanguages()
	{
		fPreferredLanguages.Clear();

		BLocaleRoster* localeRoster = BLocaleRoster::Default();
		if (localeRoster == NULL)
			return;

		BMessage preferredLanguages;
		if (localeRoster->GetPreferredLanguages(&preferredLanguages) != B_OK)
			return;

		BString language;
		int32 index = 0;
		while (preferredLanguages.FindString("language", index++,
				&language) == B_OK) {
			BString languageCode;
			language.CopyInto(languageCode, 0, 2);
				fPreferredLanguages.Add(languageCode);
		}
	}

private:
	BGroupLayout*			fRatingContainerLayout;
	RatingSummaryView*		fRatingSummaryView;
	BitmapRef				fThumbsUpIcon;
	BitmapRef				fThumbsDownIcon;
	StringList				fPreferredLanguages;
};


// #pragma mark - ContentsView


class ContentsView : public BGroupView {
public:
	ContentsView()
		:
		BGroupView("package contents view", B_HORIZONTAL)
	{
		SetViewColor(tint_color(ui_color(B_PANEL_BACKGROUND_COLOR),
			kContentTint));

		fPackageContents = new PackageContentsView("contents_list");
		AddChild(fPackageContents);

	}

	virtual ~ContentsView()
	{
	}

	virtual void Draw(BRect updateRect)
	{
	}

	void SetPackage(const PackageInfoRef& package)
	{
		fPackageContents->SetPackage(package);
	}

	void Clear()
	{
		fPackageContents->Clear();
	}

private:
	PackageContentsView*	fPackageContents;
};


// #pragma mark - ChangelogView


class ChangelogView : public BGroupView {
public:
	ChangelogView()
		:
		BGroupView("package changelog view", B_HORIZONTAL)
	{
		SetViewColor(tint_color(ui_color(B_PANEL_BACKGROUND_COLOR),
			kContentTint));

		fTextView = new MarkupTextView("changelog view");
		fTextView->SetLowColor(ViewColor());
		fTextView->SetInsets(be_plain_font->Size());

		BScrollView* scrollView = new CustomScrollView(
			"changelog scroll view", fTextView);

		BLayoutBuilder::Group<>(this)
			.Add(BSpaceLayoutItem::CreateHorizontalStrut(32.0f))
			.Add(scrollView, 1.0f)
			.SetInsets(B_USE_DEFAULT_SPACING, -1.0f, -1.0f, -1.0f)
		;
	}

	virtual ~ChangelogView()
	{
	}

	virtual void Draw(BRect updateRect)
	{
	}

	void SetPackage(const PackageInfo& package)
	{
		const BString& changelog = package.Changelog();
		if (changelog.Length() > 0)
			fTextView->SetText(changelog);
		else
			fTextView->SetDisabledText(B_TRANSLATE("No changelog available."));
	}

	void Clear()
	{
		fTextView->SetText("");
	}

private:
	MarkupTextView*		fTextView;
};


// #pragma mark - PagesView


class PagesView : public BTabView {
public:
	PagesView()
		:
		BTabView("pages view", B_WIDTH_FROM_WIDEST),
		fLayout(new BCardLayout())
	{
		SetBorder(B_NO_BORDER);

		fAboutView = new AboutView();
		fUserRatingsView = new UserRatingsView();
		fChangelogView = new ChangelogView();
		fContentsView = new ContentsView();

		AddTab(fAboutView);
		AddTab(fUserRatingsView);
		AddTab(fChangelogView);
		AddTab(fContentsView);

		TabAt(0)->SetLabel(B_TRANSLATE("About"));
		TabAt(1)->SetLabel(B_TRANSLATE("Ratings"));
		TabAt(2)->SetLabel(B_TRANSLATE("Changelog"));
		TabAt(3)->SetLabel(B_TRANSLATE("Contents"));

		Select(0);
	}

	virtual ~PagesView()
	{
		Clear();
	}

	void SetPackage(const PackageInfoRef& package, bool switchToDefaultTab)
	{
		if (switchToDefaultTab)
			Select(0);
		fAboutView->SetPackage(*package.Get());
		fUserRatingsView->SetPackage(*package.Get());
		fChangelogView->SetPackage(*package.Get());
		fContentsView->SetPackage(package);
	}

	void Clear()
	{
		fAboutView->Clear();
		fUserRatingsView->Clear();
		fChangelogView->Clear();
		fContentsView->Clear();
	}

private:
	BCardLayout*		fLayout;

	AboutView*			fAboutView;
	UserRatingsView*	fUserRatingsView;
	ChangelogView*		fChangelogView;
	ContentsView* 		fContentsView;
};


// #pragma mark - PackageInfoView


PackageInfoView::PackageInfoView(BLocker* modelLock,
		PackageActionHandler* handler)
	:
	BView("package info view", 0),
	fModelLock(modelLock),
	fPackageListener(new(std::nothrow) OnePackageMessagePackageListener(this))
{
	fCardLayout = new BCardLayout();
	SetLayout(fCardLayout);

	BGroupView* noPackageCard = new BGroupView("no package card", B_VERTICAL);
	AddChild(noPackageCard);

	BStringView* noPackageView = new BStringView("no package view",
		B_TRANSLATE("Click a package to view information"));
	noPackageView->SetHighColor(kLightBlack);
	noPackageView->SetExplicitAlignment(BAlignment(
		B_ALIGN_HORIZONTAL_CENTER, B_ALIGN_VERTICAL_CENTER));

	BLayoutBuilder::Group<>(noPackageCard)
		.Add(noPackageView)
		.SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED))
	;

	BGroupView* packageCard = new BGroupView("package card", B_VERTICAL);
	AddChild(packageCard);

	fCardLayout->SetVisibleItem((int32)0);

	fTitleView = new TitleView();
	fPackageActionView = new PackageActionView(handler);
	fPackageActionView->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED,
		B_SIZE_UNSET));
	fPagesView = new PagesView();

	BLayoutBuilder::Group<>(packageCard)
		.AddGroup(B_HORIZONTAL, 0.0f)
			.Add(fTitleView, 6.0f)
			.Add(fPackageActionView, 1.0f)
			.SetInsets(
				B_USE_DEFAULT_SPACING, 0.0f,
				B_USE_DEFAULT_SPACING, 0.0f)
		.End()
		.Add(fPagesView)
	;

	Clear();
}


PackageInfoView::~PackageInfoView()
{
	fPackageListener->SetPackage(PackageInfoRef(NULL));
	delete fPackageListener;
}


void
PackageInfoView::AttachedToWindow()
{
}


void
PackageInfoView::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case MSG_UPDATE_PACKAGE:
		{
			if (fPackageListener->Package().Get() == NULL)
				break;

			BString name;
			uint32 changes;
			if (message->FindString("name", &name) != B_OK
				|| message->FindUInt32("changes", &changes) != B_OK) {
				break;
			}

			const PackageInfoRef& package = fPackageListener->Package();
			if (package->Name() != name)
				break;

			BAutolock _(fModelLock);

			if ((changes & PKG_CHANGED_SUMMARY) != 0
				|| (changes & PKG_CHANGED_DESCRIPTION) != 0
				|| (changes & PKG_CHANGED_SCREENSHOTS) != 0) {
				fPagesView->SetPackage(package, false);
			}

			if ((changes & PKG_CHANGED_TITLE) != 0
				|| (changes & PKG_CHANGED_RATINGS) != 0) {
				fPagesView->SetPackage(package, false);
				fTitleView->SetPackage(*package.Get());
			}

			if ((changes & PKG_CHANGED_STATE) != 0) {
				fPagesView->SetPackage(package, false);
				fPackageActionView->SetPackage(*package.Get());
			}

			break;
		}
		default:
			BView::MessageReceived(message);
			break;
	}
}


void
PackageInfoView::SetPackage(const PackageInfoRef& packageRef)
{
	BAutolock _(fModelLock);

	if (packageRef.Get() == NULL) {
		Clear();
		return;
	}

	bool switchToDefaultTab = true;
	if (fPackage == packageRef) {
		// When asked to display the already showing package ref,
		// don't switch to the default tab.
		switchToDefaultTab = false;
	} else if (fPackage.Get() != NULL && packageRef.Get() != NULL
		&& fPackage->Name() == packageRef->Name()) {
		// When asked to display a different PackageInfo instance,
		// but it has the same package title as the already showing
		// instance, this probably means there was a repository
		// refresh and we are in fact still requested to show the
		// same package as before the refresh.
		switchToDefaultTab = false;
	}

	const PackageInfo& package = *packageRef.Get();

	fTitleView->SetPackage(package);
	fPackageActionView->SetPackage(package);
	fPagesView->SetPackage(packageRef, switchToDefaultTab);

	fCardLayout->SetVisibleItem(1);

	fPackageListener->SetPackage(packageRef);

	// Set the fPackage reference last, so we keep a reference to the
	// previous package before switching all the views to the new package.
	// Otherwise the PackageInfo instance may go away because we had the
	// last reference. And some of the views, the PackageActionView for
	// example, keeps references to stuff from the previous package and
	// access it while switching to the new package.
	fPackage = packageRef;
}


void
PackageInfoView::Clear()
{
	BAutolock _(fModelLock);

	fTitleView->Clear();
	fPackageActionView->Clear();
	fPagesView->Clear();

	fCardLayout->SetVisibleItem((int32)0);

	fPackageListener->SetPackage(PackageInfoRef(NULL));

	fPackage.Unset();
}

