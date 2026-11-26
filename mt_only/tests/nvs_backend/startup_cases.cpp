static int failures, total;
static void check(const char *name, bool ok) {
 ++total; if (!ok) ++failures; printf("%s %s\n", ok ? "PASS" : "FAIL", name);
}
static nvs_fs setup(std::vector<int> script) {
 static flash_parameters parameters{4, 0xff};
 comparisons = script; comparisonIndex = 0; comparisonAddresses.clear();
 recoveries = writes = erases = lockDepth = 0;
 return {0, 4096, 4, &parameters, 0, 0};
}
int main() {
 auto fs = setup({-EIO});
 int result = nvs_startup(&fs);
 check("first closing-sector comparison EIO aborts before recovery/mutation",
       result == -EIO && recoveries == 0 && writes == 0 && erases == 0 && lockDepth == 0);
 fs = setup({1, -EIO}); result = nvs_startup(&fs);
 check("next-sector comparison EIO aborts before recovery/mutation",
       result == -EIO && recoveries == 0 && writes == 0 && erases == 0 && lockDepth == 0);
 fs = setup({0, 0, 0, 0, -EIO}); result = nvs_startup(&fs);
 check("last-sector comparison EIO aborts before recovery/mutation",
       result == -EIO && recoveries == 0 && writes == 0 && erases == 0 && lockDepth == 0);
 fs = setup({0, 0, 0, 0, 0}); result = nvs_startup(&fs);
 check("healthy erased-sector startup still succeeds",
       result == 0 && recoveries == 1 && writes == 1 && erases == 0 && lockDepth == 0);
 fs = setup({1, 0}); result = nvs_startup(&fs);
 check("healthy closed/open-sector transition still succeeds",
       result == 0 && recoveries == 1 && writes == 1 && erases == 0 && lockDepth == 0);
 printf("%d/%d startup contract checks passed; no hardware/RF\n", total-failures, total);
 return failures ? 1 : 0;
}
