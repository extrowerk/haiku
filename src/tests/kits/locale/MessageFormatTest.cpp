/*
 * Copyright 2014 Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */


#include "MessageFormatTest.h"

#include <Locale.h>
#include <MessageFormat.h>

#include <cppunit/TestCaller.h>
#include <cppunit/TestSuite.h>


MessageFormatTest::MessageFormatTest()
{
}


MessageFormatTest::~MessageFormatTest()
{
}


void
MessageFormatTest::TestFormat()
{
	BString output;
	BMessageFormat formatter;

	struct Test {
		const char* locale;
		const char* pattern;
		int32 number;
		const char* expected;
	};
	
	static const char* polishTemplate = "{0, plural, one{Wybrano # obiekt} "
		"few{Wybrano # obiekty} many{Wybrano # obiektów} "
		"other{Wybrano # obyektu}}";

	static const Test tests[] = {
		{"en_US", "{0, plural, one{# dog} other{# dogs}}", 1, "1 dog"},
		{"en_US", "{0, plural, one{# dog} other{# dogs}}", 2, "2 dogs"},
		{"pl_PL", polishTemplate, 1, "Wybrano 1 obiekt"},
		{"pl_PL", polishTemplate, 3, "Wybrano 3 obyektu"},
		{"pl_PL", polishTemplate, 5, "Wybrano 5 obyektu"},
		{"pl_PL", polishTemplate, 23, "Wybrano 23 obyektu"},
		{NULL, NULL, 0, NULL}
	};

	for (int i = 0; tests[i].pattern != NULL; i++) {
		status_t result;
		NextSubTest();
		output.Truncate(0);
		BLanguage language(tests[i].locale);
		formatter.SetLanguage(language);

		result = formatter.Format(output, tests[i].pattern, tests[i].number);
		CPPUNIT_ASSERT_EQUAL(B_OK, result);
		CPPUNIT_ASSERT_EQUAL(BString(tests[i].expected), output);
	}
}


/*static*/ void
MessageFormatTest::AddTests(BTestSuite& parent)
{
	CppUnit::TestSuite& suite = *new CppUnit::TestSuite("MessageFormatTest");

	suite.addTest(new CppUnit::TestCaller<MessageFormatTest>(
		"MessageFormatTest::TestFormat", &MessageFormatTest::TestFormat));

	parent.addTest("MessageFormatTest", &suite);
}