#include <models/sentiment.hpp>
#include <cstdio>

int main() {
    using namespace altair;
    int failures = 0;
    const auto check=[&](bool ok,const char* s){std::printf("%s %s\n",ok?"PASS":"FAIL",s);if(!ok)++failures;};
    FinanceSentiment model;
    const SentimentDocument positive{100,120,7,"Profit growth beat estimates"};
    const SentimentDocument negated{100,120,7,"Profit did not beat estimates"};
    const auto a=model.score(positive,120), b=model.score(negated,120);
    check(a && a->score > 0.0 && a->matched_tokens == 3,
          "finance lexicon scores matched positive terms with entity provenance");
    check(b && b->score < a->score, "negation changes the following finance term");
    check(!model.score(positive,119), "news unavailable at decision time is refused");
    std::printf("Sentiment: %d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
